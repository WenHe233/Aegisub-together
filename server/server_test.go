package collab

import (
	"context"
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

func receiveType(t *testing.T, connection *websocket.Conn, expected string) protocol.Envelope {
	t.Helper()
	for attempts := 0; attempts < 12; attempts++ {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		messageType, data, err := connection.Read(ctx)
		cancel()
		if err != nil {
			t.Fatalf("waiting for %s: %v", expected, err)
		}
		if messageType != websocket.MessageText {
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
		operations[index] = protocol.InsertOperation{Op: "insert", LineID: fmt.Sprintf("client-%d", index), LeftID: &leftID, RightID: nil, Fields: operationFields}
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
