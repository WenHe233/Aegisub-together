package collab

import (
	"context"
	"errors"
	"time"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
)

var (
	errMaintenanceActive        = errors.New("maintenance mode is held by another member")
	errMaintenanceNotHeld       = errors.New("maintenance mode is not held by this member")
	errMaintenanceCancelTooSoon = errors.New("maintenance cancellation grace period has not elapsed")
)

type maintenanceTimeouts struct {
	idle        time.Duration
	hard        time.Duration
	cancelGrace time.Duration
}

func (server *Server) maintenanceTimeouts() maintenanceTimeouts {
	return maintenanceTimeouts{
		idle: server.config.MaintenanceIdleTimeout, hard: server.config.MaintenanceHardTimeout, cancelGrace: server.config.MaintenanceCancelGrace,
	}
}

func (hub *hub) currentMaintenance(roomID string, timeouts maintenanceTimeouts) (protocol.MaintenanceState, bool) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	if value == nil || value.maintenance == nil {
		return protocol.MaintenanceState{}, false
	}
	return maintenanceState(value, timeouts), true
}

func (hub *hub) requestMaintenance(roomID, memberID string, now time.Time, timeouts maintenanceTimeouts) (protocol.MaintenanceState, []protocol.LockState, []*member, error) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	joinedMember := memberByID(value, memberID)
	if value == nil || joinedMember == nil {
		return protocol.MaintenanceState{}, nil, nil, errors.New("room or member does not exist")
	}
	if value.maintenance != nil {
		if value.maintenance.holderID != memberID {
			return maintenanceState(value, timeouts), nil, connectedMembers(value), errMaintenanceActive
		}
		return maintenanceState(value, timeouts), nil, connectedMembers(value), nil
	}
	lease := &maintenanceLease{holderID: memberID, holderName: joinedMember.nickname, startedAt: now, lastActivity: now}
	if err := hub.store.audit(context.Background(), roomID, memberID, "maintenance_granted", map[string]any{"holder_name": joinedMember.nickname}); err != nil {
		return protocol.MaintenanceState{}, nil, nil, err
	}
	value.maintenance = lease
	states := releaseAllLocks(value)
	return maintenanceState(value, timeouts), states, connectedMembers(value), nil
}

func (hub *hub) releaseMaintenance(roomID, memberID string, now time.Time, timeouts maintenanceTimeouts) (protocol.MaintenanceState, []*member, error) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	if value == nil || memberByID(value, memberID) == nil || value.maintenance == nil || value.maintenance.holderID != memberID {
		return protocol.MaintenanceState{}, nil, errMaintenanceNotHeld
	}
	if err := hub.store.audit(context.Background(), roomID, memberID, "maintenance_released", maintenanceAuditDetails(value.maintenance, now)); err != nil {
		return protocol.MaintenanceState{}, nil, err
	}
	value.maintenance = nil
	return maintenanceState(value, timeouts), connectedMembers(value), nil
}

func (hub *hub) requestMaintenanceCancel(roomID, memberID string, now time.Time, timeouts maintenanceTimeouts) (protocol.MaintenanceState, []*member, error) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	joinedMember := memberByID(value, memberID)
	if value == nil || joinedMember == nil || value.maintenance == nil || value.maintenance.holderID == memberID {
		return protocol.MaintenanceState{}, nil, errMaintenanceNotHeld
	}
	lease := value.maintenance
	if lease.cancelRequestedBy == "" {
		if err := hub.store.audit(context.Background(), roomID, memberID, "maintenance_cancel_requested", map[string]any{"holder_id": lease.holderID}); err != nil {
			return protocol.MaintenanceState{}, nil, err
		}
		lease.cancelRequestedBy = memberID
		lease.cancelRequestedName = joinedMember.nickname
		lease.cancelRequestedAt = now
	}
	return maintenanceState(value, timeouts), connectedMembers(value), nil
}

func (hub *hub) forceMaintenanceCancel(roomID, memberID string, now time.Time, timeouts maintenanceTimeouts) (protocol.MaintenanceState, []*member, error) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	if value == nil || memberByID(value, memberID) == nil || value.maintenance == nil || value.maintenance.holderID == memberID {
		return protocol.MaintenanceState{}, nil, errMaintenanceNotHeld
	}
	lease := value.maintenance
	if lease.cancelRequestedBy != memberID || lease.cancelRequestedAt.IsZero() || now.Before(lease.cancelRequestedAt.Add(timeouts.cancelGrace)) {
		return maintenanceState(value, timeouts), connectedMembers(value), errMaintenanceCancelTooSoon
	}
	if err := hub.store.audit(context.Background(), roomID, memberID, "maintenance_force_cancelled", maintenanceAuditDetails(lease, now)); err != nil {
		return protocol.MaintenanceState{}, nil, err
	}
	value.maintenance = nil
	return maintenanceState(value, timeouts), connectedMembers(value), nil
}

func maintenanceState(value *room, timeouts maintenanceTimeouts) protocol.MaintenanceState {
	if value == nil || value.maintenance == nil {
		return protocol.MaintenanceState{Active: false}
	}
	lease := value.maintenance
	state := protocol.MaintenanceState{
		Active: true, HolderID: stringPointerValue(lease.holderID), HolderName: stringPointerValue(lease.holderName),
		StartedAt: timePointerValue(lease.startedAt), IdleExpiresAt: timePointerValue(lease.lastActivity.Add(timeouts.idle)),
		HardExpiresAt: timePointerValue(lease.startedAt.Add(timeouts.hard)),
	}
	if lease.cancelRequestedBy != "" {
		state.CancelRequestedBy = stringPointerValue(lease.cancelRequestedBy)
		state.CancelRequestedName = stringPointerValue(lease.cancelRequestedName)
		state.CancelForceAt = timePointerValue(lease.cancelRequestedAt.Add(timeouts.cancelGrace))
	}
	return state
}

func releaseAllLocks(value *room) []protocol.LockState {
	states := make([]protocol.LockState, 0, len(value.locks))
	for lineID, existing := range value.locks {
		delete(value.locks, lineID)
		states = append(states, unlockedState(lineID, existing.memberID))
	}
	return states
}

func maintenanceAuditDetails(lease *maintenanceLease, endedAt time.Time) map[string]any {
	return map[string]any{
		"holder_id": lease.holderID, "holder_name": lease.holderName,
		"started_at": lease.startedAt.UTC().Format(time.RFC3339Nano), "ended_at": endedAt.UTC().Format(time.RFC3339Nano),
	}
}

func timePointerValue(value time.Time) *string {
	formatted := value.UTC().Format(time.RFC3339Nano)
	return &formatted
}

func disconnectMaintenance(value *room, memberID string, now time.Time, store *sqliteStore) bool {
	lease := value.maintenance
	if lease == nil {
		return false
	}
	if lease.holderID == memberID {
		_ = store.audit(context.Background(), value.id, memberID, "maintenance_holder_disconnected", maintenanceAuditDetails(lease, now))
		value.maintenance = nil
		return true
	}
	if lease.cancelRequestedBy == memberID {
		lease.cancelRequestedBy = ""
		lease.cancelRequestedName = ""
		lease.cancelRequestedAt = time.Time{}
		return true
	}
	return false
}
