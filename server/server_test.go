package collab

import (
	"context"
	"encoding/json"
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
	t.Cleanup(func() { connection.CloseNow() })
	return connection
}

func send(t *testing.T, connection *websocket.Conn, messageType, requestID string, payload any) {
	t.Helper()
	data, err := json.Marshal(protocol.OutboundEnvelope{ProtocolVersion: 1, Type: messageType, RequestID: requestID, RoomRevision: 0, Payload: payload})
	if err != nil {
		t.Fatal(err)
	}
	if err := connection.Write(context.Background(), websocket.MessageText, data); err != nil {
		t.Fatal(err)
	}
}

func receive(t *testing.T, connection *websocket.Conn) protocol.Envelope {
	t.Helper()
	messageType, data, err := connection.Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if messageType != websocket.MessageText {
		t.Fatalf("unexpected message type %v", messageType)
	}
	var envelope protocol.Envelope
	if err := json.Unmarshal(data, &envelope); err != nil {
		t.Fatal(err)
	}
	return envelope
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
	if !strings.HasPrefix(newID, "srv-") || applied.Operations[1].Line == nil || applied.Operations[1].Line.LineID != newID || *applied.Operations[1].Line.Fields.Actor != "remapped" {
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
