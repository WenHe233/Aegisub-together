package collab

import (
	"errors"
	"sort"
	"time"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
	"github.com/coder/websocket"
)

type transientEvent struct {
	roomID     string
	typeName   string
	payload    any
	recipients []*member
}

func (hub *hub) requestLock(roomID, memberID, lineID string, now time.Time, idleTimeout time.Duration) ([]protocol.LockState, protocol.Presence, []*member, error) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	joinedMember := memberByID(value, memberID)
	if value == nil || joinedMember == nil || liveLine(value.snapshot.Lines, lineID) == nil {
		return nil, protocol.Presence{}, nil, errors.New("line or member does not exist")
	}
	joinedMember.activeLine = lineID
	joinedMember.lastSeen = now
	states := releaseMemberLocks(value, memberID, lineID)
	state := protocol.LockState{LineID: lineID, RequesterID: memberID}
	if existing, locked := value.locks[lineID]; locked && now.Sub(existing.lastActivity) >= idleTimeout {
		delete(value.locks, lineID)
		states = append(states, unlockedState(lineID, existing.memberID))
	}
	if !value.lockEnabled {
		state.Granted = true
	} else if existing, locked := value.locks[lineID]; locked && existing.memberID != memberID {
		state.HolderID = stringPointerValue(existing.memberID)
		state.HolderName = stringPointerValue(existing.nickname)
		remaining := idleTimeout - now.Sub(existing.lastActivity)
		if remaining < 0 {
			remaining = 0
		}
		state.ExpiresInMS = remaining.Milliseconds()
	} else {
		value.locks[lineID] = lineLock{memberID: memberID, nickname: joinedMember.nickname, lastActivity: now}
		state.Granted = true
		state.HolderID = stringPointerValue(memberID)
		state.HolderName = stringPointerValue(joinedMember.nickname)
		state.ExpiresInMS = idleTimeout.Milliseconds()
	}
	states = append(states, state)
	return states, presenceSnapshot(value), connectedMembers(value), nil
}

func (hub *hub) releaseLock(roomID, memberID, lineID string, now time.Time) ([]protocol.LockState, protocol.Presence, []*member, error) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	joinedMember := memberByID(value, memberID)
	if value == nil || joinedMember == nil {
		return nil, protocol.Presence{}, nil, errors.New("room or member does not exist")
	}
	joinedMember.lastSeen = now
	if joinedMember.activeLine == lineID {
		joinedMember.activeLine = ""
	}
	states := make([]protocol.LockState, 0, 1)
	if existing, locked := value.locks[lineID]; locked && existing.memberID == memberID {
		delete(value.locks, lineID)
		states = append(states, unlockedState(lineID, memberID))
	}
	return states, presenceSnapshot(value), connectedMembers(value), nil
}

func (hub *hub) heartbeat(roomID, memberID string, now time.Time) bool {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	joinedMember := memberByID(value, memberID)
	if joinedMember == nil {
		return false
	}
	joinedMember.lastHeartbeat = now
	return true
}

func (hub *hub) expire(now time.Time, idleTimeout, heartbeatTimeout time.Duration) ([]transientEvent, []*websocket.Conn) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	var events []transientEvent
	var staleConnections []*websocket.Conn
	for _, value := range hub.rooms {
		var states []protocol.LockState
		presenceChanged := false
		for lineID, existing := range value.locks {
			if now.Sub(existing.lastActivity) >= idleTimeout {
				delete(value.locks, lineID)
				states = append(states, unlockedState(lineID, existing.memberID))
			}
		}
		for nickname, joinedMember := range value.members {
			if joinedMember.connection != nil && now.Sub(joinedMember.lastHeartbeat) >= heartbeatTimeout {
				staleConnections = append(staleConnections, joinedMember.connection)
				states = append(states, releaseMemberLocks(value, joinedMember.id, "")...)
				delete(value.members, nickname)
				presenceChanged = true
			}
		}
		recipients := connectedMembers(value)
		for _, state := range states {
			events = append(events, transientEvent{roomID: value.id, typeName: "lock_state", payload: state, recipients: recipients})
		}
		if presenceChanged {
			events = append(events, transientEvent{roomID: value.id, typeName: "presence", payload: presenceSnapshot(value), recipients: recipients})
		}
	}
	return events, staleConnections
}

func (hub *hub) disconnect(roomID, memberID string) []transientEvent {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	joinedMember := memberByID(value, memberID)
	if value == nil || joinedMember == nil {
		return nil
	}
	delete(value.members, joinedMember.nickname)
	states := releaseMemberLocks(value, memberID, "")
	recipients := connectedMembers(value)
	events := make([]transientEvent, 0, len(states)+1)
	for _, state := range states {
		events = append(events, transientEvent{roomID: roomID, typeName: "lock_state", payload: state, recipients: recipients})
	}
	events = append(events, transientEvent{roomID: roomID, typeName: "presence", payload: presenceSnapshot(value), recipients: recipients})
	return events
}

func (hub *hub) connections() []*websocket.Conn {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	var connections []*websocket.Conn
	for _, value := range hub.rooms {
		for _, joinedMember := range value.members {
			if joinedMember.connection != nil {
				connections = append(connections, joinedMember.connection)
			}
		}
	}
	return connections
}

func releaseMemberLocks(value *room, memberID, exceptLine string) []protocol.LockState {
	var states []protocol.LockState
	for lineID, existing := range value.locks {
		if existing.memberID == memberID && lineID != exceptLine {
			delete(value.locks, lineID)
			states = append(states, unlockedState(lineID, memberID))
		}
	}
	return states
}

func presenceSnapshot(value *room) protocol.Presence {
	presence := protocol.Presence{Members: make([]protocol.PresenceMember, 0, len(value.members))}
	for _, joinedMember := range value.members {
		var lineID *string
		if joinedMember.activeLine != "" {
			lineID = stringPointerValue(joinedMember.activeLine)
		}
		presence.Members = append(presence.Members, protocol.PresenceMember{
			MemberID: joinedMember.id, Nickname: joinedMember.nickname, LineID: lineID, LastSeen: joinedMember.lastSeen.UTC().Format(time.RFC3339Nano),
		})
	}
	sort.Slice(presence.Members, func(left, right int) bool { return presence.Members[left].MemberID < presence.Members[right].MemberID })
	return presence
}

func memberByID(value *room, memberID string) *member {
	if value == nil {
		return nil
	}
	for _, joinedMember := range value.members {
		if joinedMember.id == memberID {
			return joinedMember
		}
	}
	return nil
}

func unlockedState(lineID, requesterID string) protocol.LockState {
	return protocol.LockState{LineID: lineID, RequesterID: requesterID, Granted: false}
}

func stringPointerValue(value string) *string { return &value }

func lockedByOther(value *room, lineID, memberID string) (lineLock, bool) {
	if !value.lockEnabled {
		return lineLock{}, false
	}
	existing, locked := value.locks[lineID]
	return existing, locked && existing.memberID != memberID
}
