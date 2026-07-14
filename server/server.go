package collab

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/WenHe233/Aegisub-together/server/internal/auth"
	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
	"github.com/coder/websocket"
)

const maxMessageSize = 64 << 20

type Config struct {
	AccessPasswordHash     string
	PasswordParams         auth.Params
	AuthTimeout            time.Duration
	DatabasePath           string
	LockIdleTimeout        time.Duration
	HeartbeatTimeout       time.Duration
	SweepInterval          time.Duration
	MaintenanceIdleTimeout time.Duration
	MaintenanceHardTimeout time.Duration
	MaintenanceCancelGrace time.Duration
}

type Server struct {
	hub           *hub
	config        Config
	accessLimiter *failureLimiter
	roomLimiter   *failureLimiter
	handler       http.Handler
	store         *sqliteStore
	stop          chan struct{}
	closeOnce     sync.Once
	workers       sync.WaitGroup
}

func New(config Config) (*Server, error) {
	if config.PasswordParams == (auth.Params{}) {
		config.PasswordParams = auth.DefaultParams()
	}
	if config.AuthTimeout == 0 {
		config.AuthTimeout = 5 * time.Second
	}
	if config.LockIdleTimeout == 0 {
		config.LockIdleTimeout = 60 * time.Second
	}
	if config.HeartbeatTimeout == 0 {
		config.HeartbeatTimeout = 30 * time.Second
	}
	if config.SweepInterval == 0 {
		config.SweepInterval = time.Second
	}
	if config.MaintenanceIdleTimeout == 0 {
		config.MaintenanceIdleTimeout = 10 * time.Minute
	}
	if config.MaintenanceHardTimeout == 0 {
		config.MaintenanceHardTimeout = 60 * time.Minute
	}
	if config.MaintenanceCancelGrace == 0 {
		config.MaintenanceCancelGrace = 30 * time.Second
	}
	store, err := openStore(config.DatabasePath)
	if err != nil {
		return nil, err
	}
	createdHub, err := newHub(config.PasswordParams, store)
	if err != nil {
		store.close()
		return nil, err
	}
	server := &Server{hub: createdHub, config: config, accessLimiter: newFailureLimiter(), roomLimiter: newFailureLimiter(), store: store, stop: make(chan struct{})}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", server.health)
	mux.HandleFunc("GET /v1/ws", server.websocket)
	server.handler = mux
	server.workers.Add(1)
	go server.sweepExpiredState()
	return server, nil
}

func (server *Server) Close() error {
	var closeError error
	server.closeOnce.Do(func() {
		close(server.stop)
		for _, connection := range server.hub.connections() {
			connection.CloseNow()
		}
		server.workers.Wait()
		closeError = server.store.close()
	})
	return closeError
}

func (server *Server) ServeHTTP(writer http.ResponseWriter, request *http.Request) {
	server.handler.ServeHTTP(writer, request)
}

func (server *Server) health(writer http.ResponseWriter, _ *http.Request) {
	writer.Header().Set("Content-Type", "text/plain; charset=utf-8")
	writer.WriteHeader(http.StatusOK)
	_, _ = writer.Write([]byte("ok"))
}

func (server *Server) websocket(writer http.ResponseWriter, request *http.Request) {
	remoteIP := clientIP(request)
	connection, err := websocket.Accept(writer, request, &websocket.AcceptOptions{CompressionMode: websocket.CompressionDisabled})
	if err != nil {
		return
	}
	defer connection.CloseNow()
	connection.SetReadLimit(maxMessageSize)
	if blocked, _ := server.accessLimiter.blocked(remoteIP); blocked {
		_ = connection.Close(websocket.StatusPolicyViolation, "")
		return
	}

	authContext, cancel := context.WithTimeout(context.Background(), server.config.AuthTimeout)
	authEnvelope, err := readEnvelope(authContext, connection)
	cancel()
	if err != nil || authEnvelope.Type != "access_auth" || authEnvelope.ProtocolVersion != protocol.Version {
		server.accessLimiter.fail(remoteIP)
		_ = connection.Close(websocket.StatusPolicyViolation, "")
		return
	}
	var access protocol.AccessAuth
	if err := decodePayload(authEnvelope.Payload, &access); err != nil || !server.verifyAccess(access.Password) {
		server.accessLimiter.fail(remoteIP)
		_ = connection.Close(websocket.StatusPolicyViolation, "")
		return
	}
	server.accessLimiter.success(remoteIP)
	if err := writeEnvelope(request.Context(), connection, "access_ok", authEnvelope.RequestID, 0, struct{}{}); err != nil {
		return
	}

	joinEnvelope, err := readEnvelope(request.Context(), connection)
	if err != nil || joinEnvelope.ProtocolVersion != protocol.Version {
		return
	}
	if blocked, retryAfter := server.roomLimiter.blocked(remoteIP); blocked {
		_ = writeEnvelope(request.Context(), connection, "error", joinEnvelope.RequestID, 0, protocol.Error{
			Code: "rate_limited", Message: "too many authentication failures", Retryable: true, RetryAfterMS: retryAfter.Milliseconds(),
		})
		return
	}
	joinedRoom, joinedMember, err := server.joinOrCreate(joinEnvelope)
	if err != nil {
		server.handleJoinError(request.Context(), connection, remoteIP, joinEnvelope.RequestID, err)
		return
	}
	server.roomLimiter.success(remoteIP)
	defer server.memberDisconnected(joinedRoom.id, joinedMember.id)
	server.hub.attach(joinedRoom, joinedMember, connection)

	joined, joinedRevision, ok := server.hub.joinedPayload(joinedRoom.id, joinedMember.id)
	if !ok {
		return
	}
	if err := writeEnvelope(request.Context(), connection, "room_joined", joinEnvelope.RequestID, joinedRevision, joined); err != nil {
		return
	}
	if state, active := server.hub.currentMaintenance(joinedRoom.id, server.maintenanceTimeouts()); active {
		if err := writeEnvelope(request.Context(), connection, "maintenance_state", joinEnvelope.RequestID, joinedRevision, state); err != nil {
			return
		}
	}

	for {
		envelope, err := readEnvelope(request.Context(), connection)
		if err != nil {
			return
		}
		if envelope.Type == "heartbeat" {
			if !server.hub.heartbeat(joinedRoom.id, joinedMember.id, time.Now()) {
				return
			}
			if err := writeEnvelope(request.Context(), connection, "heartbeat", envelope.RequestID, server.hub.currentRevision(joinedRoom.id), struct{}{}); err != nil {
				return
			}
			continue
		}
		if envelope.Type == "lock_request" || envelope.Type == "lock_release" {
			var reference protocol.LineReference
			if err := decodePayload(envelope.Payload, &reference); err != nil || reference.LineID == "" {
				_ = writeProtocolError(request.Context(), connection, envelope.RequestID, server.hub.currentRevision(joinedRoom.id), "invalid_message", "lock request is invalid", false)
				continue
			}
			var states []protocol.LockState
			var presence protocol.Presence
			var recipients []*member
			if envelope.Type == "lock_request" {
				states, presence, recipients, err = server.hub.requestLock(joinedRoom.id, joinedMember.id, reference.LineID, time.Now(), server.config.LockIdleTimeout)
			} else {
				states, presence, recipients, err = server.hub.releaseLock(joinedRoom.id, joinedMember.id, reference.LineID, time.Now())
			}
			if err != nil {
				if errors.Is(err, errMaintenanceActive) {
					_ = writeProtocolError(request.Context(), connection, envelope.RequestID, server.hub.currentRevision(joinedRoom.id), "maintenance_active", err.Error(), true)
				} else {
					_ = writeProtocolError(request.Context(), connection, envelope.RequestID, server.hub.currentRevision(joinedRoom.id), "invalid_message", err.Error(), false)
				}
				continue
			}
			revision := server.hub.currentRevision(joinedRoom.id)
			for _, state := range states {
				broadcastTransient(envelope.RequestID, revision, "lock_state", state, recipients)
			}
			broadcastTransient(envelope.RequestID, revision, "presence", presence, recipients)
			continue
		}
		if envelope.Type == "maintenance_request" || envelope.Type == "maintenance_release" || envelope.Type == "maintenance_cancel_request" || envelope.Type == "maintenance_cancel_force" {
			var empty struct{}
			if err := decodePayload(envelope.Payload, &empty); err != nil {
				_ = writeProtocolError(request.Context(), connection, envelope.RequestID, server.hub.currentRevision(joinedRoom.id), "invalid_message", "maintenance request is invalid", false)
				continue
			}
			now := time.Now()
			timeouts := server.maintenanceTimeouts()
			var state protocol.MaintenanceState
			var states []protocol.LockState
			var recipients []*member
			switch envelope.Type {
			case "maintenance_request":
				state, states, recipients, err = server.hub.requestMaintenance(joinedRoom.id, joinedMember.id, now, timeouts)
			case "maintenance_release":
				state, recipients, err = server.hub.releaseMaintenance(joinedRoom.id, joinedMember.id, now, timeouts)
			case "maintenance_cancel_request":
				state, recipients, err = server.hub.requestMaintenanceCancel(joinedRoom.id, joinedMember.id, now, timeouts)
			case "maintenance_cancel_force":
				state, recipients, err = server.hub.forceMaintenanceCancel(joinedRoom.id, joinedMember.id, now, timeouts)
			}
			if err != nil {
				writeMaintenanceError(request.Context(), connection, envelope.RequestID, server.hub.currentRevision(joinedRoom.id), err)
				continue
			}
			revision := server.hub.currentRevision(joinedRoom.id)
			for _, lockState := range states {
				broadcastTransient(envelope.RequestID, revision, "lock_state", lockState, recipients)
			}
			broadcastTransient(envelope.RequestID, revision, "maintenance_state", state, recipients)
			continue
		}
		if envelope.Type == "submit_batch" {
			var batch protocol.SubmitBatch
			if err := decodePayload(envelope.Payload, &batch); err != nil {
				_ = writeBatchRejected(request.Context(), connection, envelope.RequestID, server.hub.currentRevision(joinedRoom.id), batch.BatchID, &batchFailure{code: "batch_conflict", message: "batch payload is invalid"})
				continue
			}
			result, revision, recipients, duplicate, err := server.hub.applyBatch(context.Background(), joinedRoom.id, joinedMember.id, batch)
			if err != nil {
				var failure *batchFailure
				if errors.As(err, &failure) {
					_ = writeBatchRejected(request.Context(), connection, envelope.RequestID, revision, batch.BatchID, failure)
				} else {
					_ = writeProtocolError(request.Context(), connection, envelope.RequestID, revision, "internal_error", "batch could not be persisted", true)
				}
				continue
			}
			if duplicate {
				_ = writeEnvelope(request.Context(), connection, "batch_applied", envelope.RequestID, revision, result)
				continue
			}
			broadcastBatch(envelope.RequestID, revision, result, recipients)
			if state, active := server.hub.currentMaintenance(joinedRoom.id, server.maintenanceTimeouts()); active {
				broadcastTransient(envelope.RequestID, revision, "maintenance_state", state, recipients)
			}
			continue
		}
		_ = writeProtocolError(request.Context(), connection, envelope.RequestID, server.hub.currentRevision(joinedRoom.id), "invalid_message", "message is not valid in the current session", false)
	}
}

func writeMaintenanceError(ctx context.Context, connection *websocket.Conn, requestID string, revision int64, err error) {
	code, retryable := "internal_error", true
	switch {
	case errors.Is(err, errMaintenanceActive):
		code = "maintenance_active"
	case errors.Is(err, errMaintenanceNotHeld):
		code, retryable = "maintenance_not_held", false
	case errors.Is(err, errMaintenanceCancelTooSoon):
		code = "maintenance_cancel_pending"
	}
	_ = writeProtocolError(ctx, connection, requestID, revision, code, err.Error(), retryable)
}

func (server *Server) memberDisconnected(roomID, memberID string) {
	for _, event := range server.hub.disconnect(roomID, memberID, server.maintenanceTimeouts()) {
		broadcastTransient("disconnect-"+memberID, server.hub.currentRevision(roomID), event.typeName, event.payload, event.recipients)
	}
}

func (server *Server) sweepExpiredState() {
	defer server.workers.Done()
	ticker := time.NewTicker(server.config.SweepInterval)
	defer ticker.Stop()
	for {
		select {
		case now := <-ticker.C:
			events, staleConnections := server.hub.expire(now, server.config.LockIdleTimeout, server.config.HeartbeatTimeout, server.maintenanceTimeouts())
			for _, event := range events {
				broadcastTransient("lease-expired", server.hub.currentRevision(event.roomID), event.typeName, event.payload, event.recipients)
			}
			for _, connection := range staleConnections {
				_ = connection.Close(websocket.StatusGoingAway, "heartbeat timeout")
			}
		case <-server.stop:
			return
		}
	}
}

func broadcastTransient(requestID string, revision int64, typeName string, payload any, recipients []*member) {
	for _, recipient := range recipients {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		_ = writeEnvelope(ctx, recipient.connection, typeName, requestID, revision, payload)
		cancel()
	}
}

func writeBatchRejected(ctx context.Context, connection *websocket.Conn, requestID string, revision int64, batchID string, failure *batchFailure) error {
	payload := protocol.BatchRejected{BatchID: batchID, Code: failure.code, Message: failure.message, LineID: failure.lineID, OperationIndex: failure.opIndex}
	return writeEnvelope(ctx, connection, "batch_rejected", requestID, revision, payload)
}

func broadcastBatch(requestID string, revision int64, result protocol.BatchApplied, recipients []*member) {
	for _, recipient := range recipients {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		_ = writeEnvelope(ctx, recipient.connection, "batch_applied", requestID, revision, result)
		cancel()
	}
}

func (server *Server) verifyAccess(password string) bool {
	if server.config.AccessPasswordHash == "" {
		_ = auth.Verify(password, server.hub.fakeHash)
		return password == ""
	}
	return auth.Verify(password, server.config.AccessPasswordHash)
}

func (server *Server) joinOrCreate(envelope protocol.Envelope) (*room, *member, error) {
	switch envelope.Type {
	case "create_room":
		var input protocol.CreateRoom
		if err := decodePayload(envelope.Payload, &input); err != nil {
			return nil, nil, errInvalidRoom
		}
		return server.hub.create(input)
	case "join_room":
		var input protocol.JoinRoom
		if err := decodePayload(envelope.Payload, &input); err != nil {
			return nil, nil, errRoomCredentials
		}
		return server.hub.join(input)
	default:
		return nil, nil, errInvalidRoom
	}
}

func (server *Server) handleJoinError(ctx context.Context, connection *websocket.Conn, remoteIP, requestID string, err error) {
	code := "invalid_message"
	message := "invalid room request"
	retryable := false
	switch {
	case errors.Is(err, errRoomCredentials):
		server.roomLimiter.fail(remoteIP)
		code, message, retryable = "room_credentials_invalid", "room name or password is invalid", true
	case errors.Is(err, errRoomNameTaken):
		code, message = "room_name_taken", "room name is already in use"
	case errors.Is(err, errNicknameInUse):
		code, message, retryable = "nickname_in_use", "nickname is already in use", true
	}
	_ = writeProtocolError(ctx, connection, requestID, 0, code, message, retryable)
}

func readEnvelope(ctx context.Context, connection *websocket.Conn) (protocol.Envelope, error) {
	messageType, data, err := connection.Read(ctx)
	if err != nil {
		return protocol.Envelope{}, err
	}
	if messageType != websocket.MessageText {
		return protocol.Envelope{}, errors.New("expected text JSON envelope")
	}
	var envelope protocol.Envelope
	if err := decodeStrict(data, &envelope); err != nil {
		return protocol.Envelope{}, fmt.Errorf("decode envelope: %w", err)
	}
	if envelope.RequestID == "" || envelope.Payload == nil {
		return protocol.Envelope{}, errors.New("incomplete envelope")
	}
	return envelope, nil
}

func decodePayload(raw json.RawMessage, target any) error {
	return decodeStrict(raw, target)
}

func decodeStrict(raw []byte, target any) error {
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return err
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		if err == nil {
			return errors.New("trailing JSON value")
		}
		return err
	}
	return nil
}

func writeEnvelope(ctx context.Context, connection *websocket.Conn, messageType, requestID string, revision int64, payload any) error {
	envelope := protocol.OutboundEnvelope{ProtocolVersion: protocol.Version, Type: messageType, RequestID: requestID, RoomRevision: revision, Payload: payload}
	data, err := json.Marshal(envelope)
	if err != nil {
		return err
	}
	return connection.Write(ctx, websocket.MessageText, data)
}

func writeProtocolError(ctx context.Context, connection *websocket.Conn, requestID string, revision int64, code, message string, retryable bool) error {
	return writeEnvelope(ctx, connection, "error", requestID, revision, protocol.Error{Code: code, Message: message, Retryable: retryable})
}

func clientIP(request *http.Request) string {
	host, _, err := net.SplitHostPort(request.RemoteAddr)
	if err != nil {
		return request.RemoteAddr
	}
	return host
}
