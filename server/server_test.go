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
	t.Cleanup(httpServer.Close)
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
	defer httpServer.Close()
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
