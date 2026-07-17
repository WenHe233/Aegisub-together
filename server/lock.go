package collab

import (
	"context"
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

func (hub *hub) requestLockSet(roomID, memberID string, request protocol.LockSetRequest, now time.Time, idleTimeout time.Duration) ([]protocol.LockSetState, protocol.Presence, []*member, error) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	joinedMember := memberByID(value, memberID)
	if value == nil || joinedMember == nil {
		return nil, protocol.Presence{}, nil, errors.New("room or member does not exist")
	}
	if request.Generation < 0 || len(request.LineIDs) > protocol.MaximumLockSetSize {
		return nil, protocol.Presence{}, nil, errors.New("lock set is invalid or too large")
	}
	seen := make(map[string]struct{}, len(request.LineIDs))
	for _, lineID := range request.LineIDs {
		if !validLineID(lineID) || liveLine(value.snapshot.Lines, lineID) == nil {
			return nil, protocol.Presence{}, nil, errors.New("lock set contains an invalid or missing line")
		}
		if _, exists := seen[lineID]; exists {
			return nil, protocol.Presence{}, nil, errors.New("lock set contains duplicate lines")
		}
		seen[lineID] = struct{}{}
	}
	if request.ActiveLineID != nil {
		if !validLineID(*request.ActiveLineID) || liveLine(value.snapshot.Lines, *request.ActiveLineID) == nil {
			return nil, protocol.Presence{}, nil, errors.New("active line is invalid or missing")
		}
		joinedMember.activeLine = *request.ActiveLineID
	} else {
		joinedMember.activeLine = ""
	}
	joinedMember.lastSeen = now
	if value.maintenance != nil && value.maintenance.holderID != memberID {
		return nil, protocol.Presence{}, nil, errMaintenanceActive
	}

	changedMembers := expireLocks(value, now, idleTimeout)
	if !value.lockEnabled {
		changedMembers[memberID] = struct{}{}
		releaseMemberLocks(value, memberID)
		states := lockStatesForMembers(value, changedMembers, memberID, request.Generation, true, nil)
		return states, presenceSnapshot(value), connectedMembers(value), nil
	}

	conflicts := make([]protocol.LockConflict, 0)
	for _, lineID := range request.LineIDs {
		if existing, locked := value.locks[lineID]; locked && existing.memberID != memberID {
			remaining := idleTimeout - now.Sub(existing.lastActivity)
			if remaining < 0 {
				remaining = 0
			}
			conflicts = append(conflicts, protocol.LockConflict{
				LineID: lineID, HolderID: existing.memberID, HolderName: existing.nickname, ExpiresInMS: remaining.Milliseconds(),
			})
		}
	}
	sort.Slice(conflicts, func(left, right int) bool { return conflicts[left].LineID < conflicts[right].LineID })
	changedMembers[memberID] = struct{}{}
	releaseMemberLocks(value, memberID)
	if len(conflicts) != 0 {
		states := lockStatesForMembers(value, changedMembers, memberID, request.Generation, false, conflicts)
		return states, presenceSnapshot(value), connectedMembers(value), nil
	}
	for _, lineID := range request.LineIDs {
		value.locks[lineID] = lineLock{memberID: memberID, nickname: joinedMember.nickname, lastActivity: now}
	}
	states := lockStatesForMembers(value, changedMembers, memberID, request.Generation, true, nil)
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

func (hub *hub) expire(now time.Time, idleTimeout, heartbeatTimeout, resumeTimeout time.Duration, maintenanceTimeouts maintenanceTimeouts) ([]transientEvent, []*websocket.Conn) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	var events []transientEvent
	var staleConnections []*websocket.Conn
	for _, value := range hub.rooms {
		changedMembers := expireLocks(value, now, idleTimeout)
		presenceChanged := false
		maintenanceChanged := false
		if lease := value.maintenance; lease != nil && (now.Sub(lease.lastActivity) >= maintenanceTimeouts.idle || now.Sub(lease.startedAt) >= maintenanceTimeouts.hard) {
			eventType := "maintenance_idle_expired"
			if now.Sub(lease.startedAt) >= maintenanceTimeouts.hard {
				eventType = "maintenance_hard_expired"
			}
			if hub.store.audit(context.Background(), value.id, lease.holderID, eventType, maintenanceAuditDetails(lease, now)) == nil {
				value.maintenance = nil
				maintenanceChanged = true
			}
		}
		for nickname, joinedMember := range value.members {
			if joinedMember.connection != nil && now.Sub(joinedMember.lastHeartbeat) >= heartbeatTimeout {
				staleConnections = append(staleConnections, joinedMember.connection)
				if releaseMemberLocks(value, joinedMember.id) {
					changedMembers[joinedMember.id] = struct{}{}
				}
				maintenanceChanged = disconnectMaintenance(value, joinedMember.id, now, hub.store) || maintenanceChanged
				joinedMember.connection = nil
				joinedMember.disconnectedAt = now
				delete(value.members, nickname)
				presenceChanged = true
			}
		}
		for token, session := range value.sessions {
			if session.connection == nil && !session.disconnectedAt.IsZero() && now.Sub(session.disconnectedAt) >= resumeTimeout {
				delete(value.sessions, token)
			}
		}
		recipients := connectedMembers(value)
		for _, state := range lockStatesForMembers(value, changedMembers, "", 0, true, nil) {
			events = append(events, transientEvent{roomID: value.id, typeName: "lock_set_state", payload: state, recipients: recipients})
		}
		if presenceChanged {
			events = append(events, transientEvent{roomID: value.id, typeName: "presence", payload: presenceSnapshot(value), recipients: recipients})
		}
		if maintenanceChanged {
			events = append(events, transientEvent{roomID: value.id, typeName: "maintenance_state", payload: maintenanceState(value, maintenanceTimeouts), recipients: recipients})
		}
	}
	return events, staleConnections
}

func (hub *hub) disconnect(roomID, memberID string, timeouts maintenanceTimeouts) []transientEvent {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	joinedMember := memberByID(value, memberID)
	if value == nil || joinedMember == nil {
		return nil
	}
	now := time.Now()
	delete(value.members, joinedMember.nickname)
	joinedMember.connection = nil
	joinedMember.disconnectedAt = now
	released := releaseMemberLocks(value, memberID)
	maintenanceChanged := disconnectMaintenance(value, memberID, now, hub.store)
	recipients := connectedMembers(value)
	events := make([]transientEvent, 0, 3)
	if released {
		events = append(events, transientEvent{roomID: roomID, typeName: "lock_set_state", payload: protocol.LockSetState{
			MemberID: memberID, MemberName: joinedMember.nickname, Granted: true, LineIDs: []string{}, Conflicts: []protocol.LockConflict{},
		}, recipients: recipients})
	}
	events = append(events, transientEvent{roomID: roomID, typeName: "presence", payload: presenceSnapshot(value), recipients: recipients})
	if maintenanceChanged {
		events = append(events, transientEvent{roomID: roomID, typeName: "maintenance_state", payload: maintenanceState(value, timeouts), recipients: recipients})
	}
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

func releaseMemberLocks(value *room, memberID string) bool {
	released := false
	for lineID, existing := range value.locks {
		if existing.memberID == memberID {
			delete(value.locks, lineID)
			released = true
		}
	}
	return released
}

func expireLocks(value *room, now time.Time, idleTimeout time.Duration) map[string]struct{} {
	changed := make(map[string]struct{})
	for lineID, existing := range value.locks {
		if now.Sub(existing.lastActivity) >= idleTimeout {
			delete(value.locks, lineID)
			changed[existing.memberID] = struct{}{}
		}
	}
	return changed
}

func lockSetForMember(value *room, memberID string) []string {
	lineIDs := make([]string, 0)
	for lineID, existing := range value.locks {
		if existing.memberID == memberID {
			lineIDs = append(lineIDs, lineID)
		}
	}
	sort.Strings(lineIDs)
	return lineIDs
}

func lockSetsSnapshot(value *room) []protocol.LockSetState {
	members := make(map[string]struct{})
	for _, existing := range value.locks {
		members[existing.memberID] = struct{}{}
	}
	return lockStatesForMembers(value, members, "", 0, true, nil)
}

func lockStatesForMembers(value *room, members map[string]struct{}, requesterID string, generation int64, granted bool, conflicts []protocol.LockConflict) []protocol.LockSetState {
	memberIDs := make([]string, 0, len(members))
	for memberID := range members {
		memberIDs = append(memberIDs, memberID)
	}
	sort.Strings(memberIDs)
	states := make([]protocol.LockSetState, 0, len(memberIDs))
	for _, memberID := range memberIDs {
		name := "disconnected"
		if joined := memberByID(value, memberID); joined != nil {
			name = joined.nickname
		} else {
			for _, existing := range value.locks {
				if existing.memberID == memberID {
					name = existing.nickname
					break
				}
			}
		}
		state := protocol.LockSetState{MemberID: memberID, MemberName: name, Granted: true, LineIDs: lockSetForMember(value, memberID), Conflicts: []protocol.LockConflict{}}
		if memberID == requesterID {
			state.Generation = generation
			state.Granted = granted
			if !granted {
				state.LineIDs = []string{}
				state.Conflicts = append([]protocol.LockConflict(nil), conflicts...)
			}
		}
		states = append(states, state)
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

func stringPointerValue(value string) *string { return &value }

func lockOwnedBy(value *room, lineID, memberID string) (lineLock, bool) {
	if !value.lockEnabled {
		return lineLock{}, true
	}
	if value.maintenance != nil && value.maintenance.holderID == memberID {
		return lineLock{}, true
	}
	existing, locked := value.locks[lineID]
	return existing, locked && existing.memberID == memberID
}
