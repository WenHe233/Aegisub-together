package collab

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"sync"

	"github.com/WenHe233/Aegisub-together/server/internal/auth"
	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
	"github.com/coder/websocket"
	"golang.org/x/text/unicode/norm"
)

var (
	errRoomNameTaken   = errors.New("room name already exists")
	errRoomCredentials = errors.New("room name or password is invalid")
	errNicknameInUse   = errors.New("nickname already in use")
	errInvalidRoom     = errors.New("invalid room")
)

type member struct {
	id          string
	nickname    string
	resumeToken string
	connection  *websocket.Conn
}

type room struct {
	id           string
	name         string
	passwordHash string
	lockEnabled  bool
	snapshot     protocol.Snapshot
	revision     int64
	members      map[string]*member
	tombstones   map[string]protocol.Line
}

type hub struct {
	mu             sync.Mutex
	rooms          map[string]*room
	passwordParams auth.Params
	fakeHash       string
	store          *sqliteStore
}

func newHub(params auth.Params, store *sqliteStore) (*hub, error) {
	fakeHash, err := auth.Hash("fake room password", params)
	if err != nil {
		return nil, err
	}
	created := &hub{rooms: make(map[string]*room), passwordParams: params, fakeHash: fakeHash, store: store}
	rooms, err := store.loadRooms(context.Background())
	if err != nil {
		return nil, err
	}
	for _, value := range rooms {
		created.rooms[value.name] = value
	}
	return created, nil
}

func (hub *hub) create(input protocol.CreateRoom) (*room, *member, error) {
	name := norm.NFC.String(input.RoomName)
	nickname := norm.NFC.String(input.Nickname)
	if err := validateRoomInput(name, input.RoomPassword, nickname, input.Snapshot); err != nil {
		return nil, nil, err
	}
	passwordHash, err := auth.Hash(input.RoomPassword, hub.passwordParams)
	if err != nil {
		return nil, nil, err
	}

	hub.mu.Lock()
	defer hub.mu.Unlock()
	if _, exists := hub.rooms[name]; exists {
		return nil, nil, errRoomNameTaken
	}
	joinedMember, err := newMember(nickname)
	if err != nil {
		return nil, nil, err
	}
	roomID, err := secureToken(16)
	if err != nil {
		return nil, nil, err
	}
	created := &room{
		id:           roomID,
		name:         name,
		passwordHash: passwordHash,
		lockEnabled:  input.LockEnabled,
		snapshot:     cloneSnapshot(input.Snapshot),
		members:      map[string]*member{nickname: joinedMember},
		tombstones:   make(map[string]protocol.Line),
	}
	canonicalizePositions(created.snapshot.Lines)
	if err := hub.store.createRoom(context.Background(), created); err != nil {
		return nil, nil, err
	}
	hub.rooms[name] = created
	return created, joinedMember, nil
}

func (hub *hub) attach(value *room, joinedMember *member, connection *websocket.Conn) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	if existingRoom := hub.rooms[value.name]; existingRoom != nil {
		if existingMember := existingRoom.members[joinedMember.nickname]; existingMember != nil && existingMember.id == joinedMember.id {
			existingMember.connection = connection
		}
	}
}

func (hub *hub) joinedPayload(roomID, memberID string) (protocol.RoomJoined, int64, bool) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	if value == nil {
		return protocol.RoomJoined{}, 0, false
	}
	for _, joinedMember := range value.members {
		if joinedMember.id == memberID {
			return protocol.RoomJoined{
				RoomID: value.id, MemberID: joinedMember.id, ResumeToken: joinedMember.resumeToken,
				LockEnabled: value.lockEnabled, Snapshot: cloneSnapshot(value.snapshot),
			}, value.revision, true
		}
	}
	return protocol.RoomJoined{}, 0, false
}

func (hub *hub) currentRevision(roomID string) int64 {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	if value := hub.roomByID(roomID); value != nil {
		return value.revision
	}
	return 0
}

func (hub *hub) join(input protocol.JoinRoom) (*room, *member, error) {
	name := norm.NFC.String(input.RoomName)
	nickname := norm.NFC.String(input.Nickname)
	if !validName(name, 64) || !validName(nickname, 32) || len(input.RoomPassword) < 8 || len(input.RoomPassword) > 128 {
		_ = auth.Verify(input.RoomPassword, hub.fakeHash)
		return nil, nil, errRoomCredentials
	}

	hub.mu.Lock()
	defer hub.mu.Unlock()
	joinedRoom := hub.rooms[name]
	hash := hub.fakeHash
	if joinedRoom != nil {
		hash = joinedRoom.passwordHash
	}
	if !auth.Verify(input.RoomPassword, hash) || joinedRoom == nil {
		return nil, nil, errRoomCredentials
	}
	if _, exists := joinedRoom.members[nickname]; exists {
		return nil, nil, errNicknameInUse
	}
	joinedMember, err := newMember(nickname)
	if err != nil {
		return nil, nil, err
	}
	joinedRoom.members[nickname] = joinedMember
	return joinedRoom, joinedMember, nil
}

func (hub *hub) leave(roomID, nickname, memberID string) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	for _, existingRoom := range hub.rooms {
		if existingRoom.id != roomID {
			continue
		}
		existingMember := existingRoom.members[nickname]
		if existingMember != nil && existingMember.id == memberID {
			delete(existingRoom.members, nickname)
		}
		return
	}
}

func newMember(nickname string) (*member, error) {
	memberID, err := secureToken(16)
	if err != nil {
		return nil, err
	}
	resumeToken, err := secureToken(32)
	if err != nil {
		return nil, err
	}
	return &member{id: memberID, nickname: nickname, resumeToken: resumeToken}, nil
}

func validateRoomInput(name, password, nickname string, snapshot protocol.Snapshot) error {
	if !validName(name, 64) || !validName(nickname, 32) || len(password) < 8 || len(password) > 128 {
		return errInvalidRoom
	}
	if snapshot.StylesVersion < 1 || snapshot.ScriptInfoVersion < 1 {
		return errInvalidRoom
	}
	seen := make(map[string]struct{}, len(snapshot.Lines))
	positions := make(map[string]struct{}, len(snapshot.Lines))
	for _, line := range snapshot.Lines {
		if line.LineID == "" || line.PosKey == "" || len(line.PosKey) > 64 || line.Version < 1 {
			return errInvalidRoom
		}
		if _, exists := seen[line.LineID]; exists {
			return errInvalidRoom
		}
		seen[line.LineID] = struct{}{}
		if _, exists := positions[line.PosKey]; exists {
			return errInvalidRoom
		}
		positions[line.PosKey] = struct{}{}
		if !completeLineFields(line.Fields) {
			return errInvalidRoom
		}
	}
	return nil
}

func completeLineFields(fields protocol.LineFields) bool {
	if fields.Comment == nil || fields.Layer == nil || fields.StartMS == nil || fields.EndMS == nil || fields.Style == nil || fields.Actor == nil || fields.Effect == nil || fields.Text == nil {
		return false
	}
	if *fields.StartMS < 0 || *fields.EndMS < 0 || len(fields.Margins) != 3 {
		return false
	}
	for _, margin := range fields.Margins {
		if margin < 0 || margin > 9999 {
			return false
		}
	}
	return true
}

func validName(value string, max int) bool {
	count := 0
	for range value {
		count++
	}
	return count >= 1 && count <= max
}

func secureToken(size int) (string, error) {
	data := make([]byte, size)
	if _, err := rand.Read(data); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(data), nil
}

func cloneSnapshot(snapshot protocol.Snapshot) protocol.Snapshot {
	clone := snapshot
	clone.Lines = append([]protocol.Line(nil), snapshot.Lines...)
	for index := range clone.Lines {
		clone.Lines[index].Fields.Margins = append([]int(nil), snapshot.Lines[index].Fields.Margins...)
	}
	clone.Styles = append([]string(nil), snapshot.Styles...)
	clone.ScriptInfo = append([]protocol.ScriptInfoEntry(nil), snapshot.ScriptInfo...)
	clone.Comments = append([]protocol.Comment(nil), snapshot.Comments...)
	return clone
}
