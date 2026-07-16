package collab

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sort"
	"time"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
)

type batchFailure struct {
	code    string
	message string
	lineID  string
	opIndex int
}

func (failure *batchFailure) Error() string { return failure.message }

func (hub *hub) applyBatch(ctx context.Context, roomID, actorID string, input protocol.SubmitBatch) (protocol.BatchApplied, int64, []*member, bool, error) {
	if input.BatchID == "" || len(input.BatchID) > 64 || len(input.Operations) == 0 || len(input.Operations) > 100000 {
		return protocol.BatchApplied{}, 0, nil, false, &batchFailure{code: "batch_conflict", message: "batch is empty or invalid"}
	}

	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	if value == nil {
		return protocol.BatchApplied{}, 0, nil, false, errors.New("room no longer exists")
	}
	if existing, revision, found, err := hub.store.findBatch(ctx, roomID, input.BatchID); err != nil {
		return protocol.BatchApplied{}, 0, nil, false, err
	} else if found {
		return existing, revision, nil, true, nil
	}
	if value.maintenance != nil && value.maintenance.holderID != actorID {
		return protocol.BatchApplied{}, value.revision, nil, false, &batchFailure{code: "maintenance_active", message: "room is frozen for maintenance"}
	}

	working := cloneRoomState(value)
	working.revision++
	now := time.Now()
	result := protocol.BatchApplied{BatchID: input.BatchID, ActorID: actorID, IDRemap: make(map[string]string)}
	for index, raw := range input.Operations {
		applied, err := applyOperation(working, actorID, raw, result.IDRemap, now)
		if err != nil {
			var failure *batchFailure
			if errors.As(err, &failure) {
				failure.opIndex = index
			}
			return protocol.BatchApplied{}, value.revision, nil, false, err
		}
		result.Operations = append(result.Operations, applied)
	}
	if working.reindexed {
		result.Positions = make(map[string]string, len(working.snapshot.Lines))
		for _, line := range working.snapshot.Lines {
			result.Positions[line.LineID] = line.PosKey
		}
	}
	if err := hub.store.saveBatch(ctx, working, result, actorID); err != nil {
		return protocol.BatchApplied{}, value.revision, nil, false, err
	}
	value.snapshot = working.snapshot
	value.tombstones = working.tombstones
	value.revision = working.revision
	value.updatedAt = time.Now()
	if value.maintenance != nil && value.maintenance.holderID == actorID {
		value.maintenance.lastActivity = time.Now()
	}
	return result, value.revision, connectedMembers(value), false, nil
}

func applyOperation(value *room, actorID string, raw json.RawMessage, remap map[string]string, now time.Time) (protocol.AppliedOperation, error) {
	var header struct {
		Op string `json:"op"`
	}
	if err := decodeStrict(raw, &header); err != nil && header.Op == "" {
		return protocol.AppliedOperation{}, &batchFailure{code: "batch_conflict", message: "operation is malformed"}
	}
	switch header.Op {
	case "modify":
		var operation protocol.ModifyOperation
		if err := decodeStrict(raw, &operation); err != nil || !validLineID(operation.LineID) {
			return protocol.AppliedOperation{}, invalidOperation(err)
		}
		operation.LineID = remapped(operation.LineID, remap)
		if existing, owned := lockOwnedBy(value, operation.LineID, actorID); !owned {
			return protocol.AppliedOperation{}, lockFailure(operation.LineID, existing)
		}
		line, index := findLine(value.snapshot.Lines, operation.LineID)
		if line == nil || line.Version != operation.BaseVersion || !hasFields(operation.Fields) {
			return protocol.AppliedOperation{}, versionFailure(operation.LineID)
		}
		mergeFields(&line.Fields, operation.Fields)
		if !completeLineFields(line.Fields) {
			return protocol.AppliedOperation{}, invalidLine(operation.LineID)
		}
		line.Version++
		value.snapshot.Lines[index] = *line
		touchLineLock(value, operation.LineID, actorID, now)
		return applied(operation, line, 0, 0)

	case "insert":
		var operation protocol.InsertOperation
		if err := decodeStrict(raw, &operation); err != nil || !completeLineFields(operation.Fields) || !validLineID(operation.LineID) || !validOptionalLineID(operation.LeftID) || !validOptionalLineID(operation.RightID) {
			return protocol.AppliedOperation{}, invalidOperation(err)
		}
		originalID := operation.LineID
		operation.LineID = remapped(operation.LineID, remap)
		if lineExists(value, operation.LineID) {
			newID, err := mintServerLineID(value)
			if err != nil {
				return protocol.AppliedOperation{}, err
			}
			operation.LineID = newID
			remap[originalID] = operation.LineID
		}
		operation.LeftID = remappedPointer(operation.LeftID, remap)
		operation.RightID = remappedPointer(operation.RightID, remap)
		index := insertionIndex(value.snapshot.Lines, operation.LeftID, operation.RightID)
		position, reindexed := positionForInsert(value.snapshot.Lines, index)
		value.reindexed = value.reindexed || reindexed
		line := protocol.Line{LineID: operation.LineID, PosKey: position, Version: 1, Fields: operation.Fields}
		value.snapshot.Lines = insertLine(value.snapshot.Lines, index, line)
		assignLineLock(value, operation.LineID, actorID, now)
		return applied(operation, &line, 0, 0)

	case "delete":
		var operation protocol.DeleteOperation
		if err := decodeStrict(raw, &operation); err != nil || !validLineID(operation.LineID) {
			return protocol.AppliedOperation{}, invalidOperation(err)
		}
		operation.LineID = remapped(operation.LineID, remap)
		if existing, owned := lockOwnedBy(value, operation.LineID, actorID); !owned {
			return protocol.AppliedOperation{}, lockFailure(operation.LineID, existing)
		}
		line, index := findLine(value.snapshot.Lines, operation.LineID)
		if line == nil || line.Version != operation.BaseVersion {
			return protocol.AppliedOperation{}, versionFailure(operation.LineID)
		}
		value.tombstones[line.LineID] = *line
		value.snapshot.Lines = append(value.snapshot.Lines[:index], value.snapshot.Lines[index+1:]...)
		delete(value.locks, operation.LineID)
		return applied(operation, nil, 0, 0)

	case "move":
		var operation protocol.MoveOperation
		if err := decodeStrict(raw, &operation); err != nil || !validLineID(operation.LineID) || !validOptionalLineID(operation.LeftID) || !validOptionalLineID(operation.RightID) {
			return protocol.AppliedOperation{}, invalidOperation(err)
		}
		operation.LineID = remapped(operation.LineID, remap)
		operation.LeftID = remappedPointer(operation.LeftID, remap)
		operation.RightID = remappedPointer(operation.RightID, remap)
		if existing, owned := lockOwnedBy(value, operation.LineID, actorID); !owned {
			return protocol.AppliedOperation{}, lockFailure(operation.LineID, existing)
		}
		line, index := findLine(value.snapshot.Lines, operation.LineID)
		if line == nil || line.Version != operation.BaseVersion {
			return protocol.AppliedOperation{}, versionFailure(operation.LineID)
		}
		lines := append(value.snapshot.Lines[:index:index], value.snapshot.Lines[index+1:]...)
		target := insertionIndex(lines, operation.LeftID, operation.RightID)
		line.Version++
		position, reindexed := positionForInsert(lines, target)
		value.reindexed = value.reindexed || reindexed
		line.PosKey = position
		value.snapshot.Lines = insertLine(lines, target, *line)
		touchLineLock(value, operation.LineID, actorID, now)
		return applied(operation, line, 0, 0)

	case "restore":
		var operation protocol.RestoreOperation
		if err := decodeStrict(raw, &operation); err != nil || !validLineID(operation.LineID) {
			return protocol.AppliedOperation{}, invalidOperation(err)
		}
		operation.LineID = remapped(operation.LineID, remap)
		line, exists := value.tombstones[operation.LineID]
		if !exists || liveLine(value.snapshot.Lines, operation.LineID) != nil {
			return protocol.AppliedOperation{}, &batchFailure{code: "batch_conflict", message: "line cannot be restored", lineID: operation.LineID}
		}
		delete(value.tombstones, operation.LineID)
		line.Version++
		target := sort.Search(len(value.snapshot.Lines), func(index int) bool {
			return value.snapshot.Lines[index].PosKey >= line.PosKey
		})
		if target < len(value.snapshot.Lines) && value.snapshot.Lines[target].PosKey == line.PosKey {
			position, reindexed := positionForInsert(value.snapshot.Lines, target)
			value.reindexed = value.reindexed || reindexed
			line.PosKey = position
		}
		value.snapshot.Lines = insertLine(value.snapshot.Lines, target, line)
		assignLineLock(value, operation.LineID, actorID, now)
		return applied(operation, &line, 0, 0)

	case "replace_styles":
		var operation protocol.ReplaceStylesOperation
		if err := decodeStrict(raw, &operation); err != nil || len(operation.Styles) == 0 {
			return protocol.AppliedOperation{}, invalidOperation(err)
		}
		if operation.BaseVersion != value.snapshot.StylesVersion {
			return protocol.AppliedOperation{}, &batchFailure{code: "section_version_conflict", message: "styles version is stale"}
		}
		value.snapshot.Styles = append([]string(nil), operation.Styles...)
		value.snapshot.StylesVersion++
		return applied(operation, nil, value.snapshot.StylesVersion, 0)

	case "replace_script_info":
		var operation protocol.ReplaceScriptInfoOperation
		if err := decodeStrict(raw, &operation); err != nil {
			return protocol.AppliedOperation{}, invalidOperation(err)
		}
		if operation.BaseVersion != value.snapshot.ScriptInfoVersion {
			return protocol.AppliedOperation{}, &batchFailure{code: "section_version_conflict", message: "script info version is stale"}
		}
		value.snapshot.ScriptInfo = append([]protocol.ScriptInfoEntry(nil), operation.Entries...)
		value.snapshot.ScriptInfoVersion++
		return applied(operation, nil, 0, value.snapshot.ScriptInfoVersion)
	default:
		return protocol.AppliedOperation{}, &batchFailure{code: "batch_conflict", message: "unknown operation type"}
	}
}

func assignLineLock(value *room, lineID, memberID string, now time.Time) {
	if !value.lockEnabled {
		return
	}
	if joined := memberByID(value, memberID); joined != nil {
		value.locks[lineID] = lineLock{memberID: memberID, nickname: joined.nickname, lastActivity: now}
	}
}

func touchLineLock(value *room, lineID, memberID string, now time.Time) {
	if existing, ok := value.locks[lineID]; ok && existing.memberID == memberID {
		existing.lastActivity = now
		value.locks[lineID] = existing
	}
}

func applied(operation any, line *protocol.Line, stylesVersion, scriptInfoVersion int64) (protocol.AppliedOperation, error) {
	encoded, err := json.Marshal(operation)
	if err != nil {
		return protocol.AppliedOperation{}, err
	}
	return protocol.AppliedOperation{Operation: encoded, Line: line, StylesVersion: stylesVersion, ScriptInfoVersion: scriptInfoVersion}, nil
}

func invalidOperation(err error) error {
	message := "operation is invalid"
	if err != nil {
		message = fmt.Sprintf("operation is invalid: %v", err)
	}
	return &batchFailure{code: "batch_conflict", message: message}
}

func versionFailure(lineID string) error {
	return &batchFailure{code: "line_version_conflict", message: "line version is stale or the line does not exist", lineID: lineID}
}

func invalidLine(lineID string) error {
	return &batchFailure{code: "batch_conflict", message: "line fields are invalid", lineID: lineID}
}

func lockFailure(lineID string, existing lineLock) error {
	message := "line is not locked by the submitting member"
	if existing.nickname != "" {
		message = fmt.Sprintf("line is locked by %s", existing.nickname)
	}
	return &batchFailure{code: "line_locked", message: message, lineID: lineID}
}

func mergeFields(target *protocol.LineFields, update protocol.LineFields) {
	if update.Comment != nil {
		target.Comment = update.Comment
	}
	if update.Layer != nil {
		target.Layer = update.Layer
	}
	if update.StartMS != nil {
		target.StartMS = update.StartMS
	}
	if update.EndMS != nil {
		target.EndMS = update.EndMS
	}
	if update.Style != nil {
		target.Style = update.Style
	}
	if update.Actor != nil {
		target.Actor = update.Actor
	}
	if update.Effect != nil {
		target.Effect = update.Effect
	}
	if update.Margins != nil {
		target.Margins = append([]int(nil), update.Margins...)
	}
	if update.Text != nil {
		target.Text = update.Text
	}
}

func hasFields(fields protocol.LineFields) bool {
	return fields.Comment != nil || fields.Layer != nil || fields.StartMS != nil || fields.EndMS != nil || fields.Style != nil || fields.Actor != nil || fields.Effect != nil || fields.Margins != nil || fields.Text != nil
}

func findLine(lines []protocol.Line, lineID string) (*protocol.Line, int) {
	for index := range lines {
		if lines[index].LineID == lineID {
			line := lines[index]
			return &line, index
		}
	}
	return nil, -1
}

func liveLine(lines []protocol.Line, lineID string) *protocol.Line {
	line, _ := findLine(lines, lineID)
	return line
}

func lineExists(value *room, lineID string) bool {
	if liveLine(value.snapshot.Lines, lineID) != nil {
		return true
	}
	_, exists := value.tombstones[lineID]
	return exists
}

func insertLine(lines []protocol.Line, index int, line protocol.Line) []protocol.Line {
	lines = append(lines, protocol.Line{})
	copy(lines[index+1:], lines[index:])
	lines[index] = line
	return lines
}

func remapped(lineID string, remap map[string]string) string {
	if replacement := remap[lineID]; replacement != "" {
		return replacement
	}
	return lineID
}

func remappedPointer(lineID *string, remap map[string]string) *string {
	if lineID == nil {
		return nil
	}
	value := remapped(*lineID, remap)
	return &value
}

func cloneRoomState(value *room) *room {
	clone := *value
	clone.snapshot = cloneSnapshot(value.snapshot)
	clone.reindexed = false
	clone.tombstones = make(map[string]protocol.Line, len(value.tombstones))
	for lineID, line := range value.tombstones {
		clone.tombstones[lineID] = line
	}
	return &clone
}

func (hub *hub) roomByID(roomID string) *room {
	for _, value := range hub.rooms {
		if value.id == roomID {
			return value
		}
	}
	return nil
}

func connectedMembers(value *room) []*member {
	members := make([]*member, 0, len(value.members))
	for _, joined := range value.members {
		if joined.connection != nil {
			// Broadcast after releasing the hub mutex. Keep a value snapshot so a
			// concurrent disconnect cannot nil the connection under the writer.
			snapshot := *joined
			members = append(members, &snapshot)
		}
	}
	return members
}
