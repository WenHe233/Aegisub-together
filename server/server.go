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
	"time"

	"github.com/WenHe233/Aegisub-together/server/internal/auth"
	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
	"github.com/coder/websocket"
)

const maxMessageSize = 64 << 20

type Config struct {
	AccessPasswordHash string
	PasswordParams     auth.Params
	AuthTimeout        time.Duration
}

type Server struct {
	hub           *hub
	config        Config
	accessLimiter *failureLimiter
	roomLimiter   *failureLimiter
	handler       http.Handler
}

func New(config Config) (*Server, error) {
	if config.PasswordParams == (auth.Params{}) {
		config.PasswordParams = auth.DefaultParams()
	}
	if config.AuthTimeout == 0 {
		config.AuthTimeout = 5 * time.Second
	}
	createdHub, err := newHub(config.PasswordParams)
	if err != nil {
		return nil, err
	}
	server := &Server{hub: createdHub, config: config, accessLimiter: newFailureLimiter(), roomLimiter: newFailureLimiter()}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", server.health)
	mux.HandleFunc("GET /v1/ws", server.websocket)
	server.handler = mux
	return server, nil
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
	defer server.hub.leave(joinedRoom.id, joinedMember.nickname, joinedMember.id)

	joined := protocol.RoomJoined{
		RoomID:      joinedRoom.id,
		MemberID:    joinedMember.id,
		ResumeToken: joinedMember.resumeToken,
		LockEnabled: joinedRoom.lockEnabled,
		Snapshot:    cloneSnapshot(joinedRoom.snapshot),
	}
	if err := writeEnvelope(request.Context(), connection, "room_joined", joinEnvelope.RequestID, joinedRoom.revision, joined); err != nil {
		return
	}

	for {
		envelope, err := readEnvelope(request.Context(), connection)
		if err != nil {
			return
		}
		if envelope.Type == "heartbeat" {
			if err := writeEnvelope(request.Context(), connection, "heartbeat", envelope.RequestID, joinedRoom.revision, struct{}{}); err != nil {
				return
			}
			continue
		}
		_ = writeProtocolError(request.Context(), connection, envelope.RequestID, joinedRoom.revision, "invalid_message", "message is not valid in the current session", false)
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
