package collab

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/WenHe233/Aegisub-together/server/internal/auth"
	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
	"github.com/coder/websocket"
)

func rawOperation(t *testing.T, operation any) json.RawMessage {
	t.Helper()
	encoded, err := json.Marshal(operation)
	if err != nil {
		t.Fatal(err)
	}
	return encoded
}

func boolPointer(value bool) *bool       { return &value }
func intPointer(value int) *int          { return &value }
func int64Pointer(value int64) *int64    { return &value }
func stringPointer(value string) *string { return &value }

func sampleSnapshot() protocol.Snapshot {
	return protocol.Snapshot{
		Lines: []protocol.Line{{
			LineID: "9K3MT7Q2CD-1", PosKey: "V", Version: 1,
			Fields: protocol.LineFields{
				Comment: boolPointer(false), Layer: intPointer(0), StartMS: int64Pointer(0), EndMS: int64Pointer(5000),
				Style: stringPointer("Default"), Actor: stringPointer(""), Effect: stringPointer(""), Margins: []int{0, 0, 0}, Text: stringPointer("Hello"),
			},
		}},
		Styles:            []string{"Style: Default,Arial,48"},
		StylesVersion:     1,
		ScriptInfo:        []protocol.ScriptInfoEntry{{Key: "ScriptType", Value: "v4.00+"}},
		ScriptInfoVersion: 1,
		Comments:          []protocol.Comment{},
	}
}

func testServer(t *testing.T, accessPassword string) (*httptest.Server, string) {
	t.Helper()
	params := auth.Params{Memory: 64, Iterations: 1, Parallelism: 1, SaltLength: 16, KeyLength: 32}
	accessHash := ""
	if accessPassword != "" {
		var err error
		accessHash, err = auth.Hash(accessPassword, params)
		if err != nil {
			t.Fatal(err)
		}
	}
	server, err := New(Config{AccessPasswordHash: accessHash, PasswordParams: params, AuthTimeout: time.Second})
	if err != nil {
		t.Fatal(err)
	}
	httpServer := httptest.NewServer(server)
	t.Cleanup(func() {
		httpServer.Close()
		server.Close()
	})
	return httpServer, "ws" + strings.TrimPrefix(httpServer.URL, "http") + "/v1/ws"
}

func dial(t *testing.T, url string) *websocket.Conn {
	t.Helper()
	connection, _, err := websocket.Dial(context.Background(), url, nil)
	if err != nil {
		t.Fatal(err)
	}
	connection.SetReadLimit(maxMessageSize)
	t.Cleanup(func() { connection.CloseNow() })
	return connection
}

func send(t *testing.T, connection *websocket.Conn, messageType, requestID string, payload any) {
	t.Helper()
	data, err := json.Marshal(protocol.OutboundEnvelope{ProtocolVersion: 1, Type: messageType, RequestID: requestID, RoomRevision: 0, Payload: payload})
	if err != nil {
		t.Fatal(err)
	}
	frameType, frame, err := encodeWebSocketFrame(data)
	if err != nil {
		t.Fatal(err)
	}
	if err := connection.Write(context.Background(), frameType, frame); err != nil {
		t.Fatal(err)
	}
}

func receive(t *testing.T, connection *websocket.Conn) protocol.Envelope {
	t.Helper()
	envelope, _ := receiveFrame(t, connection)
	return envelope
}

func receiveFrame(t *testing.T, connection *websocket.Conn) (protocol.Envelope, websocket.MessageType) {
	t.Helper()
	messageType, data, err := connection.Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	data, err = decodeWebSocketFrame(messageType, data)
	if err != nil {
		t.Fatal(err)
	}
	var envelope protocol.Envelope
	if err := json.Unmarshal(data, &envelope); err != nil {
		t.Fatal(err)
	}
	return envelope, messageType
}

func receiveType(t *testing.T, connection *websocket.Conn, expected string) protocol.Envelope {
	t.Helper()
	for attempts := 0; attempts < 12; attempts++ {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		messageType, data, err := connection.Read(ctx)
		cancel()
		if err != nil {
			t.Fatalf("waiting for %s: %v", expected, err)
		}
		data, err = decodeWebSocketFrame(messageType, data)
		if err != nil {
			continue
		}
		var envelope protocol.Envelope
		if err := json.Unmarshal(data, &envelope); err != nil {
			t.Fatal(err)
		}
		if envelope.Type == expected {
			return envelope
		}
	}
	t.Fatalf("did not receive message type %s", expected)
	return protocol.Envelope{}
}

func configuredTestServer(t *testing.T, config Config) (*Server, *httptest.Server, string) {
	t.Helper()
	if config.PasswordParams == (auth.Params{}) {
		config.PasswordParams = auth.Params{Memory: 64, Iterations: 1, Parallelism: 1, SaltLength: 16, KeyLength: 32}
	}
	server, err := New(config)
	if err != nil {
		t.Fatal(err)
	}
	httpServer := httptest.NewServer(server)
	t.Cleanup(func() { httpServer.Close(); server.Close() })
	return server, httpServer, "ws" + strings.TrimPrefix(httpServer.URL, "http") + "/v1/ws"
}

func authenticate(t *testing.T, connection *websocket.Conn, password string) {
	t.Helper()
	send(t, connection, "access_auth", "auth-1", protocol.AccessAuth{Password: password})
	if envelope := receive(t, connection); envelope.Type != "access_ok" {
		t.Fatalf("expected access_ok, got %s", envelope.Type)
	}
}

func createRoom(t *testing.T, connection *websocket.Conn, name, nickname string) protocol.RoomJoined {
	t.Helper()
	send(t, connection, "create_room", "room-1", protocol.CreateRoom{
		RoomName: name, RoomPassword: "room password", Nickname: nickname, LockEnabled: true, Snapshot: sampleSnapshot(),
	})
	envelope := receive(t, connection)
	if envelope.Type != "room_joined" {
		t.Fatalf("expected room_joined, got %s", envelope.Type)
	}
	var joined protocol.RoomJoined
	if err := json.Unmarshal(envelope.Payload, &joined); err != nil {
		t.Fatal(err)
	}
	return joined
}

func submitBatch(t *testing.T, connection *websocket.Conn, batchID string, operations ...any) (protocol.Envelope, protocol.BatchApplied, *protocol.BatchRejected) {
	t.Helper()
	raw := make([]json.RawMessage, len(operations))
	for index, operation := range operations {
		raw[index] = rawOperation(t, operation)
	}
	send(t, connection, "submit_batch", "submit-"+batchID, protocol.SubmitBatch{BatchID: batchID, Operations: raw})
	envelope := receive(t, connection)
	if envelope.Type == "batch_rejected" {
		var rejected protocol.BatchRejected
		if err := json.Unmarshal(envelope.Payload, &rejected); err != nil {
			t.Fatal(err)
		}
		return envelope, protocol.BatchApplied{}, &rejected
	}
	if envelope.Type != "batch_applied" {
		t.Fatalf("expected batch response, got %s", envelope.Type)
	}
	var applied protocol.BatchApplied
	if err := json.Unmarshal(envelope.Payload, &applied); err != nil {
		t.Fatal(err)
	}
	return envelope, applied, nil
}

func joinRoom(t *testing.T, connection *websocket.Conn, name, nickname string) protocol.RoomJoined {
	t.Helper()
	send(t, connection, "join_room", "join-"+nickname, protocol.JoinRoom{RoomName: name, RoomPassword: "room password", Nickname: nickname})
	envelope := receive(t, connection)
	if envelope.Type != "room_joined" {
		t.Fatalf("expected room_joined, got %s", envelope.Type)
	}
	var joined protocol.RoomJoined
	if err := json.Unmarshal(envelope.Payload, &joined); err != nil {
		t.Fatal(err)
	}
	return joined
}

func TestHealthDoesNotExposeMetadata(t *testing.T) {
	server, _ := testServer(t, "")
	response, err := http.Get(server.URL + "/healthz")
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	body, _ := io.ReadAll(response.Body)
	if response.StatusCode != http.StatusOK || string(body) != "ok" {
		t.Fatalf("unexpected health response: %d %q", response.StatusCode, body)
	}
}

func TestAccessAuthenticationMustBeFirst(t *testing.T) {
	_, url := testServer(t, "server password")
	connection := dial(t, url)
	send(t, connection, "join_room", "bad-first", protocol.JoinRoom{})
	_, _, err := connection.Read(context.Background())
	if websocket.CloseStatus(err) != websocket.StatusPolicyViolation {
		t.Fatalf("expected policy violation close, got %v", err)
	}
}

func TestAccessAuthenticationRejectsWrongPasswordWithoutPayload(t *testing.T) {
	_, url := testServer(t, "server password")
	connection := dial(t, url)
	send(t, connection, "access_auth", "auth-bad", protocol.AccessAuth{Password: "wrong"})
	_, _, err := connection.Read(context.Background())
	if websocket.CloseStatus(err) != websocket.StatusPolicyViolation {
		t.Fatalf("expected policy violation close, got %v", err)
	}
}

func TestCreateAndJoinRoomReturnsSnapshot(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	created := createRoom(t, creator, "episode-01", "translator")
	if created.Snapshot.Lines[0].Fields.Text == nil || *created.Snapshot.Lines[0].Fields.Text != "Hello" {
		t.Fatal("created room did not return the initial snapshot")
	}

	joiner := dial(t, url)
	authenticate(t, joiner, "")
	send(t, joiner, "join_room", "join-1", protocol.JoinRoom{RoomName: "episode-01", RoomPassword: "room password", Nickname: "proofreader"})
	envelope := receive(t, joiner)
	if envelope.Type != "room_joined" {
		t.Fatalf("expected room_joined, got %s", envelope.Type)
	}
	var joined protocol.RoomJoined
	if err := json.Unmarshal(envelope.Payload, &joined); err != nil {
		t.Fatal(err)
	}
	if joined.RoomID != created.RoomID || joined.MemberID == created.MemberID || joined.ResumeToken == "" {
		t.Fatal("join response did not contain stable room and unique member data")
	}
}

func TestMissingRoomAndWrongPasswordUseSameError(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")

	getError := func(roomName, password, nickname string) protocol.Error {
		connection := dial(t, url)
		authenticate(t, connection, "")
		send(t, connection, "join_room", "join-fail", protocol.JoinRoom{RoomName: roomName, RoomPassword: password, Nickname: nickname})
		envelope := receive(t, connection)
		var protocolError protocol.Error
		if err := json.Unmarshal(envelope.Payload, &protocolError); err != nil {
			t.Fatal(err)
		}
		return protocolError
	}
	missing := getError("does-not-exist", "room password", "one")
	wrong := getError("episode-01", "wrong password", "two")
	if missing.Code != "room_credentials_invalid" || missing.Code != wrong.Code || missing.Message != wrong.Message {
		t.Fatalf("credential errors differ: %#v %#v", missing, wrong)
	}
}

func TestNicknameIsUniqueWithinRoom(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")

	joiner := dial(t, url)
	authenticate(t, joiner, "")
	send(t, joiner, "join_room", "join-duplicate", protocol.JoinRoom{RoomName: "episode-01", RoomPassword: "room password", Nickname: "translator"})
	envelope := receive(t, joiner)
	var protocolError protocol.Error
	if err := json.Unmarshal(envelope.Payload, &protocolError); err != nil {
		t.Fatal(err)
	}
	if protocolError.Code != "nickname_in_use" {
		t.Fatalf("unexpected error code %q", protocolError.Code)
	}
}

func TestRoomNamesAreNFCNormalized(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "café", "one")

	second := dial(t, url)
	authenticate(t, second, "")
	send(t, second, "create_room", "room-duplicate", protocol.CreateRoom{RoomName: "cafe\u0301", RoomPassword: "room password", Nickname: "two", LockEnabled: true, Snapshot: sampleSnapshot()})
	envelope := receive(t, second)
	var protocolError protocol.Error
	if err := json.Unmarshal(envelope.Payload, &protocolError); err != nil {
		t.Fatal(err)
	}
	if protocolError.Code != "room_name_taken" {
		t.Fatalf("expected normalized duplicate rejection, got %q", protocolError.Code)
	}
}

func TestSuccessfulAccessDoesNotClearRoomFailures(t *testing.T) {
	params := auth.Params{Memory: 64, Iterations: 1, Parallelism: 1, SaltLength: 16, KeyLength: 32}
	server, err := New(Config{PasswordParams: params, AuthTimeout: time.Second})
	if err != nil {
		t.Fatal(err)
	}
	server.roomLimiter.limit = 2
	httpServer := httptest.NewServer(server)
	defer func() { httpServer.Close(); server.Close() }()
	url := "ws" + strings.TrimPrefix(httpServer.URL, "http") + "/v1/ws"

	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")

	for attempt := 0; attempt < 2; attempt++ {
		connection := dial(t, url)
		authenticate(t, connection, "")
		send(t, connection, "join_room", "join-fail", protocol.JoinRoom{RoomName: "episode-01", RoomPassword: "wrong password", Nickname: "attacker"})
		receive(t, connection)
	}
	if blocked, _ := server.roomLimiter.blocked("127.0.0.1"); !blocked {
		t.Fatal("room failure count was cleared by successful access authentication")
	}
}

func TestInitialSnapshotRequiresCompleteLineFields(t *testing.T) {
	_, url := testServer(t, "")
	connection := dial(t, url)
	authenticate(t, connection, "")
	snapshot := sampleSnapshot()
	snapshot.Lines[0].Fields.Text = nil
	send(t, connection, "create_room", "invalid-snapshot", protocol.CreateRoom{
		RoomName: "episode-01", RoomPassword: "room password", Nickname: "translator", LockEnabled: true, Snapshot: snapshot,
	})
	envelope := receive(t, connection)
	var protocolError protocol.Error
	if err := json.Unmarshal(envelope.Payload, &protocolError); err != nil {
		t.Fatal(err)
	}
	if protocolError.Code != "invalid_message" {
		t.Fatalf("expected invalid_message, got %q", protocolError.Code)
	}
}

func TestInitialSnapshotRejectsNonCanonicalLineID(t *testing.T) {
	_, url := testServer(t, "")
	connection := dial(t, url)
	authenticate(t, connection, "")
	snapshot := sampleSnapshot()
	snapshot.Lines[0].LineID = "srv-legacy"
	send(t, connection, "create_room", "invalid-line-id", protocol.CreateRoom{
		RoomName: "episode-01", RoomPassword: "room password", Nickname: "translator", LockEnabled: true, Snapshot: snapshot,
	})
	envelope := receive(t, connection)
	var protocolError protocol.Error
	if err := json.Unmarshal(envelope.Payload, &protocolError); err != nil {
		t.Fatal(err)
	}
	if protocolError.Code != "invalid_message" {
		t.Fatalf("expected invalid_message, got %q", protocolError.Code)
	}
}

func TestBatchRejectsNonCanonicalLineID(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	fields := sampleSnapshot().Lines[0].Fields
	_, _, rejected := submitBatch(t, creator, "invalid-line-id", protocol.InsertOperation{
		Op: "insert", LineID: "9K3MT7Q2CD-01", LeftID: stringPointer("9K3MT7Q2CD-1"), Fields: fields,
	})
	if rejected == nil || rejected.Code != "batch_conflict" {
		t.Fatalf("expected batch_conflict, got %#v", rejected)
	}
}

func TestAtomicBatchModifiesAndBroadcastsCanonicalLine(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	observer := dial(t, url)
	authenticate(t, observer, "")
	joinRoom(t, observer, "episode-01", "proofreader")

	envelope, applied, rejected := submitBatch(t, creator, "batch-1", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("你好")},
	})
	if rejected != nil || envelope.RoomRevision != 1 || len(applied.Operations) != 1 {
		t.Fatalf("unexpected apply result: %#v %#v", applied, rejected)
	}
	line := applied.Operations[0].Line
	if line == nil || line.Version != 2 || line.Fields.Text == nil || *line.Fields.Text != "你好" || len(line.PosKey) != rankWidth {
		t.Fatal("canonical line did not include normalized position and version")
	}
	observerEnvelope := receive(t, observer)
	if observerEnvelope.Type != "batch_applied" || observerEnvelope.RoomRevision != 1 {
		t.Fatalf("observer did not receive batch broadcast: %#v", observerEnvelope)
	}
}

func TestRejectedBatchDoesNotApplyEarlierOperations(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")

	_, _, rejected := submitBatch(t, creator, "batch-reject",
		protocol.ModifyOperation{Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("must rollback")}},
		protocol.ModifyOperation{Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 99, Fields: protocol.LineFields{Actor: stringPointer("stale")}},
	)
	if rejected == nil || rejected.Code != "line_version_conflict" || rejected.OperationIndex != 1 {
		t.Fatalf("unexpected rejection: %#v", rejected)
	}

	observer := dial(t, url)
	authenticate(t, observer, "")
	joined := joinRoom(t, observer, "episode-01", "proofreader")
	if joined.Snapshot.Lines[0].Version != 1 || *joined.Snapshot.Lines[0].Fields.Text != "Hello" {
		t.Fatal("rejected batch partially changed authoritative state")
	}
}

func TestDuplicateIDIsRemappedWithinBatch(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	fields := sampleSnapshot().Lines[0].Fields
	fields.Text = stringPointer("inserted")

	_, applied, rejected := submitBatch(t, creator, "batch-remap",
		protocol.InsertOperation{Op: "insert", LineID: "9K3MT7Q2CD-1", LeftID: stringPointer("9K3MT7Q2CD-1"), RightID: nil, Fields: fields},
		protocol.ModifyOperation{Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Actor: stringPointer("remapped")}},
	)
	if rejected != nil {
		t.Fatalf("batch was rejected: %#v", rejected)
	}
	newID := applied.IDRemap["9K3MT7Q2CD-1"]
	if !validLineID(newID) || newID == "9K3MT7Q2CD-1" || applied.Operations[1].Line == nil || applied.Operations[1].Line.LineID != newID || *applied.Operations[1].Line.Fields.Actor != "remapped" {
		t.Fatalf("ID remap was not applied to later operations: %#v", applied)
	}
}

func TestDeleteAndRestorePreserveIdentity(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	_, _, rejected := submitBatch(t, creator, "batch-delete", protocol.DeleteOperation{Op: "delete", LineID: "9K3MT7Q2CD-1", BaseVersion: 1})
	if rejected != nil {
		t.Fatal(rejected)
	}
	_, restored, rejected := submitBatch(t, creator, "batch-restore", protocol.RestoreOperation{Op: "restore", LineID: "9K3MT7Q2CD-1"})
	if rejected != nil || restored.Operations[0].Line == nil || restored.Operations[0].Line.LineID != "9K3MT7Q2CD-1" || restored.Operations[0].Line.Version != 2 {
		t.Fatalf("restore did not preserve identity: %#v %#v", restored, rejected)
	}
}

func TestSectionVersionsAreAtomic(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	_, applied, rejected := submitBatch(t, creator, "batch-sections",
		protocol.ReplaceStylesOperation{Op: "replace_styles", BaseVersion: 1, Styles: []string{"Style: Default,Arial,50"}},
		protocol.ReplaceScriptInfoOperation{Op: "replace_script_info", BaseVersion: 1, Entries: []protocol.ScriptInfoEntry{{Key: "ScriptType", Value: "v4.00+"}, {Key: "PlayResX", Value: "1920"}}},
	)
	if rejected != nil || applied.Operations[0].StylesVersion != 2 || applied.Operations[1].ScriptInfoVersion != 2 {
		t.Fatalf("section versions were not advanced: %#v %#v", applied, rejected)
	}
}

func TestBatchIDIsIdempotent(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	operation := protocol.ModifyOperation{Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("once")}}
	firstEnvelope, _, firstRejected := submitBatch(t, creator, "same-batch", operation)
	secondEnvelope, second, secondRejected := submitBatch(t, creator, "same-batch", operation)
	if firstRejected != nil || secondRejected != nil || firstEnvelope.RoomRevision != 1 || secondEnvelope.RoomRevision != 1 || second.Operations[0].Line.Version != 2 {
		t.Fatal("duplicate batch was applied more than once")
	}
}

func TestSQLiteRestoresRoomsAndBatchesAfterRestart(t *testing.T) {
	databasePath := t.TempDir() + "/collab.db"
	params := auth.Params{Memory: 64, Iterations: 1, Parallelism: 1, SaltLength: 16, KeyLength: 32}
	start := func() (*Server, *httptest.Server, string) {
		server, err := New(Config{PasswordParams: params, AuthTimeout: time.Second, DatabasePath: databasePath})
		if err != nil {
			t.Fatal(err)
		}
		httpServer := httptest.NewServer(server)
		return server, httpServer, "ws" + strings.TrimPrefix(httpServer.URL, "http") + "/v1/ws"
	}

	firstServer, firstHTTP, firstURL := start()
	creator := dial(t, firstURL)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	submitBatch(t, creator, "persisted-batch", protocol.ModifyOperation{Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("persisted")}})
	creator.CloseNow()
	firstHTTP.Close()
	if err := firstServer.Close(); err != nil {
		t.Fatal(err)
	}

	secondServer, secondHTTP, secondURL := start()
	defer func() { secondHTTP.Close(); secondServer.Close() }()
	joiner := dial(t, secondURL)
	authenticate(t, joiner, "")
	joined := joinRoom(t, joiner, "episode-01", "proofreader")
	if joined.Snapshot.Lines[0].Version != 2 || *joined.Snapshot.Lines[0].Fields.Text != "persisted" {
		t.Fatal("room snapshot was not restored from SQLite")
	}
	duplicateEnvelope, _, rejected := submitBatch(t, joiner, "persisted-batch", protocol.ModifyOperation{Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("again")}})
	if rejected != nil || duplicateEnvelope.RoomRevision != 1 {
		t.Fatal("persisted batch idempotency record was not restored")
	}
}

func TestDenseBatchCarriesAtomicReindexMap(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	fields := sampleSnapshot().Lines[0].Fields
	leftID := "9K3MT7Q2CD-1"
	operations := make([]any, 200)
	for index := range operations {
		operationFields := fields
		operationFields.Text = stringPointer(fmt.Sprintf("line %d", index))
		operations[index] = protocol.InsertOperation{Op: "insert", LineID: fmt.Sprintf("0000000001-%d", index+1), LeftID: &leftID, RightID: nil, Fields: operationFields}
	}
	_, applied, rejected := submitBatch(t, creator, "dense-batch", operations...)
	if rejected != nil {
		t.Fatalf("dense batch rejected: %#v", rejected)
	}
	if len(applied.Positions) != 201 {
		t.Fatalf("reindex map has %d positions, expected 201", len(applied.Positions))
	}
	seen := make(map[string]struct{}, len(applied.Positions))
	for _, position := range applied.Positions {
		if len(position) > 64 {
			t.Fatalf("position exceeds wire limit: %q", position)
		}
		if _, duplicate := seen[position]; duplicate {
			t.Fatalf("duplicate reindexed position %q", position)
		}
		seen[position] = struct{}{}
	}
}

func TestLineLockDeniesOtherMemberAndRejectsBatch(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{})
	owner := dial(t, url)
	authenticate(t, owner, "")
	ownerJoined := createRoom(t, owner, "episode-01", "translator")
	other := dial(t, url)
	authenticate(t, other, "")
	otherJoined := joinRoom(t, other, "episode-01", "proofreader")

	send(t, owner, "lock_request", "lock-owner", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	ownerStateEnvelope := receiveType(t, owner, "lock_state")
	receiveType(t, owner, "presence")
	receiveType(t, other, "lock_state")
	receiveType(t, other, "presence")
	var ownerState protocol.LockState
	json.Unmarshal(ownerStateEnvelope.Payload, &ownerState)
	if !ownerState.Granted || ownerState.HolderID == nil || *ownerState.HolderID != ownerJoined.MemberID {
		t.Fatalf("owner did not acquire lock: %#v", ownerState)
	}

	send(t, other, "lock_request", "lock-other", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	deniedEnvelope := receiveType(t, other, "lock_state")
	receiveType(t, other, "presence")
	// Drain the same broadcast from the owner before later assertions.
	receiveType(t, owner, "lock_state")
	receiveType(t, owner, "presence")
	var denied protocol.LockState
	json.Unmarshal(deniedEnvelope.Payload, &denied)
	if denied.Granted || denied.HolderID == nil || *denied.HolderID != ownerJoined.MemberID || denied.RequesterID != otherJoined.MemberID {
		t.Fatalf("other member was not denied with holder details: %#v", denied)
	}

	_, _, rejected := submitBatch(t, other, "locked-batch", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("must not apply")},
	})
	if rejected == nil || rejected.Code != "line_locked" || rejected.LineID != "9K3MT7Q2CD-1" {
		t.Fatalf("locked batch was not rejected: %#v", rejected)
	}
}

func TestFocusChangeReleasesPreviousLock(t *testing.T) {
	snapshot := sampleSnapshot()
	second := snapshot.Lines[0]
	second.LineID = "9K3MT7Q2CD-2"
	second.PosKey = "z"
	snapshot.Lines = append(snapshot.Lines, second)
	_, _, url := configuredTestServer(t, Config{})
	owner := dial(t, url)
	authenticate(t, owner, "")
	send(t, owner, "create_room", "room-two-lines", protocol.CreateRoom{RoomName: "episode-01", RoomPassword: "room password", Nickname: "translator", LockEnabled: true, Snapshot: snapshot})
	receive(t, owner)
	other := dial(t, url)
	authenticate(t, other, "")
	joinRoom(t, other, "episode-01", "proofreader")

	send(t, owner, "lock_request", "lock-first", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	receiveType(t, owner, "presence")
	receiveType(t, other, "presence")
	send(t, owner, "lock_request", "lock-second", protocol.LineReference{LineID: "9K3MT7Q2CD-2"})
	receiveType(t, owner, "presence")
	receiveType(t, other, "presence")

	send(t, other, "lock_request", "lock-released", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	grantedEnvelope := receiveType(t, other, "lock_state")
	var granted protocol.LockState
	json.Unmarshal(grantedEnvelope.Payload, &granted)
	if !granted.Granted {
		t.Fatalf("previous focus lock was not released: %#v", granted)
	}
}

func TestInsertedLineIsLeasedToCreatorAndActivityRenewsLease(t *testing.T) {
	server, _, url := configuredTestServer(t, Config{})
	owner := dial(t, url)
	authenticate(t, owner, "")
	created := createRoom(t, owner, "episode-01", "translator")
	other := dial(t, url)
	authenticate(t, other, "")
	joinRoom(t, other, "episode-01", "proofreader")

	fields := sampleSnapshot().Lines[0].Fields
	_, _, rejected := submitBatch(t, owner, "insert-owned", protocol.InsertOperation{
		Op: "insert", LineID: "9K3MT7Q2CD-2", LeftID: stringPointer("9K3MT7Q2CD-1"), Fields: fields,
	})
	if rejected != nil {
		t.Fatalf("creator insert was rejected: %#v", rejected)
	}
	receiveType(t, other, "batch_applied")

	server.hub.mu.Lock()
	value := server.hub.roomByID(created.RoomID)
	insertedLock, exists := value.locks["9K3MT7Q2CD-2"]
	server.hub.mu.Unlock()
	if !exists || insertedLock.memberID != created.MemberID {
		t.Fatalf("inserted line was not leased to its creator: %#v", insertedLock)
	}

	time.Sleep(time.Millisecond)
	_, _, rejected = submitBatch(t, owner, "touch-owned", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-2", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("updated")},
	})
	if rejected != nil {
		t.Fatalf("creator could not edit inserted line: %#v", rejected)
	}
	receiveType(t, other, "batch_applied")
	server.hub.mu.Lock()
	renewed := value.locks["9K3MT7Q2CD-2"].lastActivity
	server.hub.mu.Unlock()
	if !renewed.After(insertedLock.lastActivity) {
		t.Fatal("editing an owned line did not renew its idle lease")
	}

	_, _, rejected = submitBatch(t, owner, "delete-owned", protocol.DeleteOperation{
		Op: "delete", LineID: "9K3MT7Q2CD-2", BaseVersion: 2,
	})
	if rejected != nil {
		t.Fatalf("creator could not delete inserted line: %#v", rejected)
	}
	receiveType(t, other, "batch_applied")
	server.hub.mu.Lock()
	_, exists = value.locks["9K3MT7Q2CD-2"]
	server.hub.mu.Unlock()
	if exists {
		t.Fatal("deleting a line left a stale line lock")
	}
}

func TestLockExpiresDespiteConnectionHeartbeat(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{LockIdleTimeout: 60 * time.Millisecond, HeartbeatTimeout: time.Second, SweepInterval: 5 * time.Millisecond})
	owner := dial(t, url)
	authenticate(t, owner, "")
	createRoom(t, owner, "episode-01", "translator")
	observer := dial(t, url)
	authenticate(t, observer, "")
	joinRoom(t, observer, "episode-01", "proofreader")
	send(t, owner, "lock_request", "lock-owner", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	receiveType(t, owner, "presence")
	receiveType(t, observer, "presence")

	time.Sleep(30 * time.Millisecond)
	send(t, owner, "heartbeat", "heartbeat-owner", struct{}{})
	receiveType(t, owner, "heartbeat")
	expiredEnvelope := receiveType(t, observer, "lock_state")
	var expired protocol.LockState
	json.Unmarshal(expiredEnvelope.Payload, &expired)
	if expired.Granted || expired.HolderID != nil || expired.LineID != "9K3MT7Q2CD-1" {
		t.Fatalf("idle lock did not expire independently of heartbeat: %#v", expired)
	}
}

func TestHeartbeatTimeoutDisconnectsMember(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{LockIdleTimeout: time.Second, HeartbeatTimeout: 50 * time.Millisecond, SweepInterval: 5 * time.Millisecond})
	connection := dial(t, url)
	authenticate(t, connection, "")
	createRoom(t, connection, "episode-01", "translator")
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_, _, err := connection.Read(ctx)
	if websocket.CloseStatus(err) != websocket.StatusGoingAway {
		t.Fatalf("expected heartbeat timeout close, got %v", err)
	}
}

func TestLockDisabledKeepsPresenceWithoutHardLock(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{})
	first := dial(t, url)
	authenticate(t, first, "")
	send(t, first, "create_room", "room-unlocked", protocol.CreateRoom{RoomName: "episode-01", RoomPassword: "room password", Nickname: "translator", LockEnabled: false, Snapshot: sampleSnapshot()})
	receive(t, first)
	second := dial(t, url)
	authenticate(t, second, "")
	joinRoom(t, second, "episode-01", "proofreader")
	send(t, first, "lock_request", "presence-first", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	stateEnvelope := receiveType(t, second, "lock_state")
	receiveType(t, second, "presence")
	var state protocol.LockState
	json.Unmarshal(stateEnvelope.Payload, &state)
	if !state.Granted || state.HolderID != nil {
		t.Fatalf("lock-disabled room created a hard holder: %#v", state)
	}
	_, _, rejected := submitBatch(t, second, "unlocked-batch", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("allowed")},
	})
	if rejected != nil {
		t.Fatalf("presence blocked edit in lock-disabled room: %#v", rejected)
	}
}

func TestMaintenanceClearsLocksFreezesOthersAndReleases(t *testing.T) {
	server, _, url := configuredTestServer(t, Config{})
	owner := dial(t, url)
	authenticate(t, owner, "")
	ownerJoined := createRoom(t, owner, "episode-01", "translator")
	holder := dial(t, url)
	authenticate(t, holder, "")
	holderJoined := joinRoom(t, holder, "episode-01", "timer")

	send(t, owner, "lock_request", "lock-owner", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	receiveType(t, owner, "lock_state")
	receiveType(t, owner, "presence")
	receiveType(t, holder, "lock_state")
	receiveType(t, holder, "presence")

	send(t, holder, "maintenance_request", "maintenance-start", struct{}{})
	unlockedEnvelope := receiveType(t, owner, "lock_state")
	ownerMaintenance := receiveType(t, owner, "maintenance_state")
	receiveType(t, holder, "lock_state")
	holderMaintenance := receiveType(t, holder, "maintenance_state")
	var unlocked protocol.LockState
	var state protocol.MaintenanceState
	if err := json.Unmarshal(unlockedEnvelope.Payload, &unlocked); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(holderMaintenance.Payload, &state); err != nil {
		t.Fatal(err)
	}
	if unlocked.Granted || unlocked.LineID != "9K3MT7Q2CD-1" || !state.Active || state.HolderID == nil || *state.HolderID != holderJoined.MemberID {
		t.Fatalf("maintenance did not clear locks and identify holder: %#v %#v", unlocked, state)
	}
	var ownerState protocol.MaintenanceState
	if err := json.Unmarshal(ownerMaintenance.Payload, &ownerState); err != nil || ownerState.HolderName == nil || *ownerState.HolderName != "timer" {
		t.Fatalf("owner did not receive maintenance state: %#v %v", ownerState, err)
	}

	_, _, rejected := submitBatch(t, owner, "frozen-batch", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("blocked")},
	})
	if rejected == nil || rejected.Code != "maintenance_active" {
		t.Fatalf("non-holder batch was not frozen: %#v", rejected)
	}
	send(t, holder, "maintenance_release", "maintenance-release", struct{}{})
	releasedEnvelope := receiveType(t, owner, "maintenance_state")
	receiveType(t, holder, "maintenance_state")
	if err := json.Unmarshal(releasedEnvelope.Payload, &state); err != nil || state.Active {
		t.Fatalf("maintenance did not release: %#v %v", state, err)
	}
	_, _, rejected = submitBatch(t, owner, "after-maintenance", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("allowed")},
	})
	if rejected != nil {
		t.Fatalf("edit remained frozen after maintenance release: %#v", rejected)
	}

	var grants, releases int
	if err := server.store.db.QueryRow(`SELECT COUNT(*) FROM audit_log WHERE room_id = ? AND event_type = 'maintenance_granted'`, ownerJoined.RoomID).Scan(&grants); err != nil {
		t.Fatal(err)
	}
	if err := server.store.db.QueryRow(`SELECT COUNT(*) FROM audit_log WHERE room_id = ? AND event_type = 'maintenance_released'`, ownerJoined.RoomID).Scan(&releases); err != nil {
		t.Fatal(err)
	}
	if grants != 1 || releases != 1 {
		t.Fatalf("maintenance audit events missing: grants=%d releases=%d", grants, releases)
	}
}

func TestMaintenanceCancelRequiresRequestAndGracePeriod(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{MaintenanceCancelGrace: 50 * time.Millisecond})
	holder := dial(t, url)
	authenticate(t, holder, "")
	createRoom(t, holder, "episode-01", "timer")
	requester := dial(t, url)
	authenticate(t, requester, "")
	joinRoom(t, requester, "episode-01", "translator")

	send(t, holder, "maintenance_request", "maintenance-start", struct{}{})
	receiveType(t, holder, "maintenance_state")
	receiveType(t, requester, "maintenance_state")
	send(t, requester, "maintenance_cancel_request", "cancel-request", struct{}{})
	receiveType(t, holder, "maintenance_state")
	cancelStateEnvelope := receiveType(t, requester, "maintenance_state")
	var cancelState protocol.MaintenanceState
	if err := json.Unmarshal(cancelStateEnvelope.Payload, &cancelState); err != nil {
		t.Fatal(err)
	}
	if cancelState.CancelRequestedName == nil || *cancelState.CancelRequestedName != "translator" || cancelState.CancelForceAt == nil {
		t.Fatalf("cancel request was not published: %#v", cancelState)
	}

	send(t, requester, "maintenance_cancel_force", "cancel-too-soon", struct{}{})
	errorEnvelope := receiveType(t, requester, "error")
	var protocolError protocol.Error
	if err := json.Unmarshal(errorEnvelope.Payload, &protocolError); err != nil {
		t.Fatal(err)
	}
	if protocolError.Code != "maintenance_cancel_pending" || !protocolError.Retryable {
		t.Fatalf("early force cancel returned wrong error: %#v", protocolError)
	}
	time.Sleep(60 * time.Millisecond)
	send(t, requester, "maintenance_cancel_force", "cancel-force", struct{}{})
	forcedEnvelope := receiveType(t, requester, "maintenance_state")
	receiveType(t, holder, "maintenance_state")
	var forced protocol.MaintenanceState
	if err := json.Unmarshal(forcedEnvelope.Payload, &forced); err != nil || forced.Active {
		t.Fatalf("force cancellation did not end maintenance: %#v %v", forced, err)
	}
}

func TestMaintenanceIdleTimeoutIgnoresHeartbeat(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{
		MaintenanceIdleTimeout: 60 * time.Millisecond, MaintenanceHardTimeout: time.Second,
		HeartbeatTimeout: time.Second, SweepInterval: 5 * time.Millisecond,
	})
	holder := dial(t, url)
	authenticate(t, holder, "")
	createRoom(t, holder, "episode-01", "timer")
	observer := dial(t, url)
	authenticate(t, observer, "")
	joinRoom(t, observer, "episode-01", "translator")
	send(t, holder, "maintenance_request", "maintenance-start", struct{}{})
	receiveType(t, holder, "maintenance_state")
	receiveType(t, observer, "maintenance_state")

	time.Sleep(30 * time.Millisecond)
	send(t, holder, "heartbeat", "maintenance-heartbeat", struct{}{})
	receiveType(t, holder, "heartbeat")
	expiredEnvelope := receiveType(t, observer, "maintenance_state")
	var expired protocol.MaintenanceState
	if err := json.Unmarshal(expiredEnvelope.Payload, &expired); err != nil || expired.Active {
		t.Fatalf("heartbeat incorrectly renewed maintenance: %#v %v", expired, err)
	}
}

func TestMaintenanceSuccessfulBatchRenewsIdleButNotHardLimit(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{
		MaintenanceIdleTimeout: 70 * time.Millisecond, MaintenanceHardTimeout: 110 * time.Millisecond,
		HeartbeatTimeout: time.Second, SweepInterval: 5 * time.Millisecond,
	})
	holder := dial(t, url)
	authenticate(t, holder, "")
	createRoom(t, holder, "episode-01", "timer")
	observer := dial(t, url)
	authenticate(t, observer, "")
	joinRoom(t, observer, "episode-01", "translator")
	send(t, holder, "maintenance_request", "maintenance-start", struct{}{})
	receiveType(t, holder, "maintenance_state")
	receiveType(t, observer, "maintenance_state")

	time.Sleep(50 * time.Millisecond)
	_, _, rejected := submitBatch(t, holder, "maintenance-batch", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("renewed")},
	})
	if rejected != nil {
		t.Fatalf("holder batch was rejected: %#v", rejected)
	}
	receiveType(t, holder, "maintenance_state")
	receiveType(t, observer, "batch_applied")
	activeEnvelope := receiveType(t, observer, "maintenance_state")
	var active protocol.MaintenanceState
	if err := json.Unmarshal(activeEnvelope.Payload, &active); err != nil || !active.Active {
		t.Fatalf("successful batch did not publish renewed lease: %#v %v", active, err)
	}
	expiredEnvelope := receiveType(t, observer, "maintenance_state")
	var expired protocol.MaintenanceState
	if err := json.Unmarshal(expiredEnvelope.Payload, &expired); err != nil || expired.Active {
		t.Fatalf("hard limit was renewed by batch activity: %#v %v", expired, err)
	}
}

func TestMaintenanceHolderDisconnectReleasesImmediately(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{HeartbeatTimeout: time.Second})
	holder := dial(t, url)
	authenticate(t, holder, "")
	createRoom(t, holder, "episode-01", "timer")
	observer := dial(t, url)
	authenticate(t, observer, "")
	joinRoom(t, observer, "episode-01", "translator")
	send(t, holder, "maintenance_request", "maintenance-start", struct{}{})
	receiveType(t, holder, "maintenance_state")
	receiveType(t, observer, "maintenance_state")
	holder.CloseNow()

	releasedEnvelope := receiveType(t, observer, "maintenance_state")
	var released protocol.MaintenanceState
	if err := json.Unmarshal(releasedEnvelope.Payload, &released); err != nil || released.Active {
		t.Fatalf("holder disconnect did not release maintenance: %#v %v", released, err)
	}
}

func TestMemberJoiningDuringMaintenanceReceivesCurrentState(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{})
	holder := dial(t, url)
	authenticate(t, holder, "")
	holderJoined := createRoom(t, holder, "episode-01", "timer")
	send(t, holder, "maintenance_request", "maintenance-start", struct{}{})
	receiveType(t, holder, "maintenance_state")

	joiner := dial(t, url)
	authenticate(t, joiner, "")
	joinRoom(t, joiner, "episode-01", "translator")
	stateEnvelope := receiveType(t, joiner, "maintenance_state")
	var state protocol.MaintenanceState
	if err := json.Unmarshal(stateEnvelope.Payload, &state); err != nil || !state.Active || state.HolderID == nil || *state.HolderID != holderJoined.MemberID {
		t.Fatalf("joining member did not receive active maintenance: %#v %v", state, err)
	}
}

func TestCommentCreationBypassesLockAndSurvivesLineDeletion(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{})
	owner := dial(t, url)
	authenticate(t, owner, "")
	createRoom(t, owner, "episode-01", "translator")
	reviewer := dial(t, url)
	authenticate(t, reviewer, "")
	joinRoom(t, reviewer, "episode-01", "proofreader")

	send(t, owner, "lock_request", "lock-owner", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	receiveType(t, owner, "lock_state")
	receiveType(t, owner, "presence")
	receiveType(t, reviewer, "lock_state")
	receiveType(t, reviewer, "presence")
	send(t, reviewer, "comment_create", "comment-create", protocol.CommentCreate{
		LineID: "9K3MT7Q2CD-1", BaseLineVersion: 1, Body: "Please shorten this.", SuggestedText: stringPointer("Shorter"),
	})
	createdEnvelope := receiveType(t, reviewer, "comment_changed")
	receiveType(t, owner, "comment_changed")
	var created protocol.CommentChanged
	if err := json.Unmarshal(createdEnvelope.Payload, &created); err != nil {
		t.Fatal(err)
	}
	if created.Comment.State != "open" || created.Comment.AuthorName != "proofreader" || created.Comment.BaseLineVersion != 1 {
		t.Fatalf("comment was not created canonically: %#v", created)
	}

	_, _, rejected := submitBatch(t, owner, "delete-commented-line", protocol.DeleteOperation{Op: "delete", LineID: "9K3MT7Q2CD-1", BaseVersion: 1})
	if rejected != nil {
		t.Fatalf("line delete failed: %#v", rejected)
	}
	joiner := dial(t, url)
	authenticate(t, joiner, "")
	joined := joinRoom(t, joiner, "episode-01", "timer")
	if len(joined.Snapshot.Lines) != 0 || len(joined.Snapshot.Comments) != 1 || joined.Snapshot.Comments[0].CommentID != created.Comment.CommentID {
		t.Fatalf("comment did not remain attached to tombstoned line: %#v", joined.Snapshot)
	}
}

func TestAcceptSuggestionRequiresLockAndAtomicallyUpdatesLine(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{})
	owner := dial(t, url)
	authenticate(t, owner, "")
	createRoom(t, owner, "episode-01", "translator")
	reviewer := dial(t, url)
	authenticate(t, reviewer, "")
	joinRoom(t, reviewer, "episode-01", "proofreader")
	send(t, reviewer, "comment_create", "comment-create", protocol.CommentCreate{
		LineID: "9K3MT7Q2CD-1", BaseLineVersion: 1, Body: "Use this wording.", SuggestedText: stringPointer("Accepted text"),
	})
	createdEnvelope := receiveType(t, reviewer, "comment_changed")
	receiveType(t, owner, "comment_changed")
	var created protocol.CommentChanged
	json.Unmarshal(createdEnvelope.Payload, &created)

	send(t, owner, "comment_set_state", "accept-without-lock", protocol.CommentSetState{CommentID: created.Comment.CommentID, State: "accepted"})
	errorEnvelope := receiveType(t, owner, "error")
	var protocolError protocol.Error
	json.Unmarshal(errorEnvelope.Payload, &protocolError)
	if protocolError.Code != "line_locked" {
		t.Fatalf("accept without lock returned %#v", protocolError)
	}
	send(t, owner, "lock_request", "lock-for-accept", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	receiveType(t, owner, "lock_state")
	receiveType(t, owner, "presence")
	receiveType(t, reviewer, "lock_state")
	receiveType(t, reviewer, "presence")
	send(t, owner, "comment_set_state", "accept-with-lock", protocol.CommentSetState{CommentID: created.Comment.CommentID, State: "accepted"})
	acceptedEnvelope := receiveType(t, owner, "comment_changed")
	receiveType(t, reviewer, "comment_changed")
	var accepted protocol.CommentChanged
	if err := json.Unmarshal(acceptedEnvelope.Payload, &accepted); err != nil {
		t.Fatal(err)
	}
	if accepted.Comment.State != "accepted" || accepted.Comment.ResolvedBy == nil || accepted.Line == nil || accepted.Line.Version != 2 || *accepted.Line.Fields.Text != "Accepted text" || acceptedEnvelope.RoomRevision != 2 {
		t.Fatalf("suggestion was not accepted atomically: %#v", accepted)
	}

	send(t, owner, "snapshot_request", "snapshot-after-accept", protocol.SnapshotRequest{AfterRevision: 0})
	snapshotEnvelope := receiveType(t, owner, "snapshot_state")
	var snapshot protocol.SnapshotState
	if err := json.Unmarshal(snapshotEnvelope.Payload, &snapshot); err != nil {
		t.Fatal(err)
	}
	if snapshot.Revision != 2 || snapshot.Snapshot.Lines[0].Version != 2 || snapshot.Snapshot.Comments[0].State != "accepted" {
		t.Fatalf("snapshot did not capture atomic suggestion acceptance: %#v", snapshot)
	}

	send(t, owner, "audit_request", "audit-comments", protocol.AuditRequest{AfterID: 0, Limit: 200})
	auditEnvelope := receiveType(t, owner, "audit_page")
	var page protocol.AuditPage
	if err := json.Unmarshal(auditEnvelope.Payload, &page); err != nil {
		t.Fatal(err)
	}
	if len(page.Entries) != 2 || page.Entries[0].EventType != "comment_created" || page.Entries[0].RoomRevision != 1 || page.Entries[1].EventType != "comment_accepted" || page.Entries[1].RoomRevision != 2 {
		t.Fatalf("comment audit trail is incomplete: %#v", page)
	}
}

func TestStaleSuggestionCannotBeAccepted(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{})
	owner := dial(t, url)
	authenticate(t, owner, "")
	createRoom(t, owner, "episode-01", "translator")
	reviewer := dial(t, url)
	authenticate(t, reviewer, "")
	joinRoom(t, reviewer, "episode-01", "proofreader")
	send(t, reviewer, "comment_create", "comment-create", protocol.CommentCreate{
		LineID: "9K3MT7Q2CD-1", BaseLineVersion: 1, Body: "Old suggestion", SuggestedText: stringPointer("Stale text"),
	})
	createdEnvelope := receiveType(t, reviewer, "comment_changed")
	receiveType(t, owner, "comment_changed")
	var created protocol.CommentChanged
	json.Unmarshal(createdEnvelope.Payload, &created)
	send(t, owner, "lock_request", "lock-owner", protocol.LineReference{LineID: "9K3MT7Q2CD-1"})
	receiveType(t, owner, "lock_state")
	receiveType(t, owner, "presence")
	receiveType(t, reviewer, "lock_state")
	receiveType(t, reviewer, "presence")
	_, _, rejected := submitBatch(t, owner, "change-before-accept", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("Newer edit")},
	})
	if rejected != nil {
		t.Fatal(rejected)
	}
	send(t, owner, "comment_set_state", "accept-stale", protocol.CommentSetState{CommentID: created.Comment.CommentID, State: "accepted"})
	errorEnvelope := receiveType(t, owner, "error")
	var protocolError protocol.Error
	json.Unmarshal(errorEnvelope.Payload, &protocolError)
	if protocolError.Code != "comment_version_conflict" {
		t.Fatalf("stale suggestion returned %#v", protocolError)
	}
}

func TestResumeTokenRestoresMemberIdentity(t *testing.T) {
	server, _, url := configuredTestServer(t, Config{})
	first := dial(t, url)
	authenticate(t, first, "")
	joined := createRoom(t, first, "episode-01", "translator")
	first.CloseNow()
	deadline := time.Now().Add(time.Second)
	for {
		server.hub.mu.Lock()
		room := server.hub.roomByID(joined.RoomID)
		active := memberByID(room, joined.MemberID) != nil
		server.hub.mu.Unlock()
		if !active {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("member did not disconnect in time")
		}
		time.Sleep(5 * time.Millisecond)
	}

	resumed := dial(t, url)
	authenticate(t, resumed, "")
	send(t, resumed, "join_room", "resume-member", protocol.JoinRoom{
		RoomName: "episode-01", RoomPassword: "room password", Nickname: "translator", ResumeToken: joined.ResumeToken,
	})
	envelope := receiveType(t, resumed, "room_joined")
	var resumedJoined protocol.RoomJoined
	if err := json.Unmarshal(envelope.Payload, &resumedJoined); err != nil {
		t.Fatal(err)
	}
	if resumedJoined.MemberID != joined.MemberID || resumedJoined.ResumeToken != joined.ResumeToken {
		t.Fatalf("resume token did not restore identity: before=%#v after=%#v", joined, resumedJoined)
	}
}

func TestExpiredResumeTokenCreatesNewMemberIdentity(t *testing.T) {
	server, _, url := configuredTestServer(t, Config{ResumeTimeout: 40 * time.Millisecond, SweepInterval: 5 * time.Millisecond})
	first := dial(t, url)
	authenticate(t, first, "")
	joined := createRoom(t, first, "episode-01", "translator")
	first.CloseNow()
	deadline := time.Now().Add(time.Second)
	for {
		server.hub.mu.Lock()
		active := memberByID(server.hub.roomByID(joined.RoomID), joined.MemberID) != nil
		server.hub.mu.Unlock()
		if !active {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("member did not disconnect in time")
		}
		time.Sleep(5 * time.Millisecond)
	}
	time.Sleep(50 * time.Millisecond)

	connection := dial(t, url)
	authenticate(t, connection, "")
	send(t, connection, "join_room", "expired-resume", protocol.JoinRoom{
		RoomName: "episode-01", RoomPassword: "room password", Nickname: "translator", ResumeToken: joined.ResumeToken,
	})
	envelope := receiveType(t, connection, "room_joined")
	var replacement protocol.RoomJoined
	if err := json.Unmarshal(envelope.Payload, &replacement); err != nil {
		t.Fatal(err)
	}
	if replacement.MemberID == joined.MemberID || replacement.ResumeToken == joined.ResumeToken {
		t.Fatalf("expired resume token restored stale identity: %#v", replacement)
	}
}

func TestCommentChangesDoNotRenewMaintenanceIdleLease(t *testing.T) {
	_, _, url := configuredTestServer(t, Config{
		MaintenanceIdleTimeout: 70 * time.Millisecond, MaintenanceHardTimeout: time.Second,
		HeartbeatTimeout: time.Second, SweepInterval: 5 * time.Millisecond,
	})
	holder := dial(t, url)
	authenticate(t, holder, "")
	createRoom(t, holder, "episode-01", "timer")
	observer := dial(t, url)
	authenticate(t, observer, "")
	joinRoom(t, observer, "episode-01", "translator")
	send(t, holder, "maintenance_request", "maintenance-start", struct{}{})
	receiveType(t, holder, "maintenance_state")
	receiveType(t, observer, "maintenance_state")
	time.Sleep(40 * time.Millisecond)
	send(t, holder, "comment_create", "maintenance-comment", protocol.CommentCreate{
		LineID: "9K3MT7Q2CD-1", BaseLineVersion: 1, Body: "This is not a subtitle batch.",
	})
	receiveType(t, holder, "comment_changed")
	receiveType(t, observer, "comment_changed")
	expiredEnvelope := receiveType(t, observer, "maintenance_state")
	var expired protocol.MaintenanceState
	if err := json.Unmarshal(expiredEnvelope.Payload, &expired); err != nil || expired.Active {
		t.Fatalf("comment incorrectly renewed maintenance lease: %#v %v", expired, err)
	}
}

func TestCommentsPersistAcrossSQLiteRestart(t *testing.T) {
	databasePath := t.TempDir() + "/comments.db"
	params := auth.Params{Memory: 64, Iterations: 1, Parallelism: 1, SaltLength: 16, KeyLength: 32}
	start := func() (*Server, *httptest.Server, string) {
		server, err := New(Config{PasswordParams: params, AuthTimeout: time.Second, DatabasePath: databasePath})
		if err != nil {
			t.Fatal(err)
		}
		httpServer := httptest.NewServer(server)
		return server, httpServer, "ws" + strings.TrimPrefix(httpServer.URL, "http") + "/v1/ws"
	}
	firstServer, firstHTTP, firstURL := start()
	creator := dial(t, firstURL)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	send(t, creator, "comment_create", "persist-comment", protocol.CommentCreate{LineID: "9K3MT7Q2CD-1", BaseLineVersion: 1, Body: "Persist me"})
	receiveType(t, creator, "comment_changed")
	creator.CloseNow()
	firstHTTP.Close()
	if err := firstServer.Close(); err != nil {
		t.Fatal(err)
	}

	secondServer, secondHTTP, secondURL := start()
	defer func() { secondHTTP.Close(); secondServer.Close() }()
	joiner := dial(t, secondURL)
	authenticate(t, joiner, "")
	joined := joinRoom(t, joiner, "episode-01", "proofreader")
	if len(joined.Snapshot.Comments) != 1 || joined.Snapshot.Comments[0].Body != "Persist me" || joined.Snapshot.Comments[0].State != "open" {
		t.Fatalf("comment did not survive restart: %#v", joined.Snapshot.Comments)
	}
}

func TestColdArchiveRestoresSnapshotTombstonesAndCredentials(t *testing.T) {
	databasePath := t.TempDir() + "/archive.db"
	params := auth.Params{Memory: 64, Iterations: 1, Parallelism: 1, SaltLength: 16, KeyLength: 32}
	start := func() (*Server, *httptest.Server, string) {
		server, err := New(Config{PasswordParams: params, AuthTimeout: time.Second, DatabasePath: databasePath})
		if err != nil {
			t.Fatal(err)
		}
		httpServer := httptest.NewServer(server)
		return server, httpServer, "ws" + strings.TrimPrefix(httpServer.URL, "http") + "/v1/ws"
	}
	firstServer, firstHTTP, firstURL := start()
	creator := dial(t, firstURL)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	_, _, rejected := submitBatch(t, creator, "archive-modify", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("Archived text")},
	})
	if rejected != nil {
		t.Fatal(rejected)
	}
	send(t, creator, "comment_create", "archive-comment", protocol.CommentCreate{LineID: "9K3MT7Q2CD-1", BaseLineVersion: 2, Body: "Archived comment"})
	receiveType(t, creator, "comment_changed")
	_, _, rejected = submitBatch(t, creator, "archive-delete", protocol.DeleteOperation{Op: "delete", LineID: "9K3MT7Q2CD-1", BaseVersion: 2})
	if rejected != nil {
		t.Fatal(rejected)
	}
	creator.CloseNow()
	firstHTTP.Close()
	if err := firstServer.Close(); err != nil {
		t.Fatal(err)
	}

	if err := SetRoomArchived(context.Background(), databasePath, "episode-01", true); err != nil {
		t.Fatal(err)
	}
	stats, err := RoomStats(context.Background(), databasePath)
	if err != nil {
		t.Fatal(err)
	}
	if len(stats) != 1 || !stats[0].Archived || stats[0].Comments != 1 || stats[0].BatchRecords != 0 || stats[0].ArchiveBytes == 0 {
		t.Fatalf("cold archive stats are wrong: %#v", stats)
	}

	secondServer, secondHTTP, secondURL := start()
	defer func() { secondHTTP.Close(); secondServer.Close() }()
	wrong := dial(t, secondURL)
	authenticate(t, wrong, "")
	send(t, wrong, "join_room", "wrong-archive-password", protocol.JoinRoom{RoomName: "episode-01", RoomPassword: "wrong password", Nickname: "attacker"})
	if envelope := receiveType(t, wrong, "error"); envelope.Type != "error" {
		t.Fatal("wrong password unexpectedly restored archive")
	}
	secondServer.hub.mu.Lock()
	stillArchived := secondServer.hub.rooms["episode-01"].archived
	secondServer.hub.mu.Unlock()
	if !stillArchived {
		t.Fatal("wrong credentials restored the archived room")
	}

	joiner := dial(t, secondURL)
	authenticate(t, joiner, "")
	joined := joinRoom(t, joiner, "episode-01", "proofreader")
	if len(joined.Snapshot.Lines) != 0 || len(joined.Snapshot.Comments) != 1 || joined.Snapshot.Comments[0].Body != "Archived comment" {
		t.Fatalf("archive snapshot was not restored: %#v", joined.Snapshot)
	}
	_, restored, rejected := submitBatch(t, joiner, "restore-from-archive", protocol.RestoreOperation{Op: "restore", LineID: "9K3MT7Q2CD-1"})
	if rejected != nil || restored.Operations[0].Line == nil || *restored.Operations[0].Line.Fields.Text != "Archived text" {
		t.Fatalf("archived tombstone was not restored: %#v %#v", restored, rejected)
	}
}

func TestSQLiteOnlineBackupIsRecoverable(t *testing.T) {
	directory := t.TempDir()
	databasePath := directory + "/source.db"
	backupPath := directory + "/backup.db"
	params := auth.Params{Memory: 64, Iterations: 1, Parallelism: 1, SaltLength: 16, KeyLength: 32}
	server, httpServer, url := configuredTestServer(t, Config{PasswordParams: params, DatabasePath: databasePath})
	creator := dial(t, url)
	authenticate(t, creator, "")
	createRoom(t, creator, "episode-01", "translator")
	_, _, rejected := submitBatch(t, creator, "backup-batch", protocol.ModifyOperation{
		Op: "modify", LineID: "9K3MT7Q2CD-1", BaseVersion: 1, Fields: protocol.LineFields{Text: stringPointer("Backed up")},
	})
	if rejected != nil {
		t.Fatal(rejected)
	}
	if err := BackupDatabase(context.Background(), databasePath, backupPath); err != nil {
		t.Fatal(err)
	}
	creator.CloseNow()
	httpServer.Close()
	if err := server.Close(); err != nil {
		t.Fatal(err)
	}
	if err := BackupDatabase(context.Background(), databasePath, backupPath); err == nil {
		t.Fatal("backup overwrote an existing destination")
	}

	restoredServer, err := New(Config{PasswordParams: params, DatabasePath: backupPath})
	if err != nil {
		t.Fatal(err)
	}
	restoredHTTP := httptest.NewServer(restoredServer)
	defer func() { restoredHTTP.Close(); restoredServer.Close() }()
	connection := dial(t, "ws"+strings.TrimPrefix(restoredHTTP.URL, "http")+"/v1/ws")
	authenticate(t, connection, "")
	joined := joinRoom(t, connection, "episode-01", "proofreader")
	if *joined.Snapshot.Lines[0].Fields.Text != "Backed up" || joined.Snapshot.Lines[0].Version != 2 {
		t.Fatalf("backup did not restore authoritative state: %#v", joined.Snapshot)
	}
}

func TestAutomaticArchiveSkipsActiveRoomAndRestoresOnJoin(t *testing.T) {
	server, _, url := configuredTestServer(t, Config{
		ArchiveAfter: 50 * time.Millisecond, ArchiveSweepInterval: 5 * time.Millisecond,
		HeartbeatTimeout: time.Second,
	})
	creator := dial(t, url)
	authenticate(t, creator, "")
	joined := createRoom(t, creator, "episode-01", "translator")
	time.Sleep(70 * time.Millisecond)
	server.hub.mu.Lock()
	archivedWhileActive := server.hub.roomByID(joined.RoomID).archived
	server.hub.mu.Unlock()
	if archivedWhileActive {
		t.Fatal("active room was automatically archived")
	}
	creator.CloseNow()

	deadline := time.Now().Add(time.Second)
	for {
		server.hub.mu.Lock()
		archived := server.hub.roomByID(joined.RoomID).archived
		server.hub.mu.Unlock()
		if archived {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("inactive room was not automatically archived")
		}
		time.Sleep(5 * time.Millisecond)
	}
	connection := dial(t, url)
	authenticate(t, connection, "")
	restored := joinRoom(t, connection, "episode-01", "proofreader")
	if len(restored.Snapshot.Lines) != 1 || *restored.Snapshot.Lines[0].Fields.Text != "Hello" {
		t.Fatalf("automatic archive did not restore on join: %#v", restored.Snapshot)
	}
}

func TestStoreMigratesPreArchiveSchema(t *testing.T) {
	databasePath := t.TempDir() + "/legacy.db"
	database, err := sql.Open("sqlite", databasePath)
	if err != nil {
		t.Fatal(err)
	}
	statements := []string{
		`CREATE TABLE rooms (id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE, password_hash TEXT NOT NULL, lock_enabled INTEGER NOT NULL, revision INTEGER NOT NULL, snapshot_json BLOB NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)`,
		`CREATE TABLE audit_log (id INTEGER PRIMARY KEY AUTOINCREMENT, room_id TEXT, actor_id TEXT, event_type TEXT NOT NULL, details_json BLOB NOT NULL, created_at TEXT NOT NULL)`,
	}
	for _, statement := range statements {
		if _, err := database.Exec(statement); err != nil {
			database.Close()
			t.Fatal(err)
		}
	}
	if err := database.Close(); err != nil {
		t.Fatal(err)
	}
	store, err := openStore(databasePath)
	if err != nil {
		t.Fatal(err)
	}
	defer store.close()
	for _, check := range []struct{ table, column string }{{"rooms", "archived_at"}, {"rooms", "archive_blob"}, {"audit_log", "room_revision"}} {
		rows, err := store.db.Query(`PRAGMA table_info(` + check.table + `)`)
		if err != nil {
			t.Fatal(err)
		}
		found := false
		for rows.Next() {
			var index int
			var name, columnType string
			var notNull, primaryKey int
			var defaultValue any
			if err := rows.Scan(&index, &name, &columnType, &notNull, &defaultValue, &primaryKey); err != nil {
				rows.Close()
				t.Fatal(err)
			}
			found = found || name == check.column
		}
		rows.Close()
		if !found {
			t.Fatalf("migration did not add %s.%s", check.table, check.column)
		}
	}
}
