package collab

import (
	"bytes"
	"compress/zlib"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
)

var (
	errRoomNotFound = errors.New("room does not exist")
)

type archivePayload struct {
	Snapshot   protocol.Snapshot        `json:"snapshot"`
	Tombstones map[string]protocol.Line `json:"tombstones"`
}

type RoomStat struct {
	Name         string `json:"name"`
	Revision     int64  `json:"revision"`
	Archived     bool   `json:"archived"`
	UpdatedAt    string `json:"updated_at"`
	LiveLines    int    `json:"live_lines"`
	Comments     int    `json:"comments"`
	BatchRecords int    `json:"batch_records"`
	AuditRecords int    `json:"audit_records"`
	ArchiveBytes int64  `json:"archive_bytes"`
}

func encodeArchive(payload archivePayload) ([]byte, error) {
	encoded, err := json.Marshal(payload)
	if err != nil {
		return nil, err
	}
	var output bytes.Buffer
	writer, err := zlib.NewWriterLevel(&output, zlib.BestCompression)
	if err != nil {
		return nil, err
	}
	if _, err := writer.Write(encoded); err != nil {
		return nil, err
	}
	if err := writer.Close(); err != nil {
		return nil, err
	}
	return output.Bytes(), nil
}

func decodeArchive(encoded []byte) (archivePayload, error) {
	reader, err := zlib.NewReader(bytes.NewReader(encoded))
	if err != nil {
		return archivePayload{}, err
	}
	defer reader.Close()
	limited := io.LimitReader(reader, maxMessageSize+1)
	data, err := io.ReadAll(limited)
	if err != nil {
		return archivePayload{}, err
	}
	if len(data) > maxMessageSize {
		return archivePayload{}, errors.New("archive exceeds decompression limit")
	}
	var payload archivePayload
	if err := json.Unmarshal(data, &payload); err != nil {
		return archivePayload{}, err
	}
	if payload.Tombstones == nil {
		payload.Tombstones = make(map[string]protocol.Line)
	}
	return payload, nil
}

func (store *sqliteStore) archiveRoom(ctx context.Context, value *room, actorID string) error {
	if value.archived {
		return nil
	}
	blob, err := encodeArchive(archivePayload{Snapshot: cloneSnapshot(value.snapshot), Tombstones: value.tombstones})
	if err != nil {
		return err
	}
	tx, err := store.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if _, err := tx.ExecContext(ctx, `UPDATE rooms SET snapshot_json = '{}', archived_at = ?, archive_blob = ?, updated_at = ? WHERE id = ?`, now, blob, now, value.id); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM tombstones WHERE room_id = ?`, value.id); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM batches WHERE room_id = ?`, value.id); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM audit_log WHERE room_id = ? AND event_type = 'batch_applied'`, value.id); err != nil {
		return err
	}
	details, _ := json.Marshal(map[string]any{"archive_bytes": len(blob), "revision": value.revision})
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO audit_log (room_id, room_revision, actor_id, event_type, details_json, created_at) VALUES (?, ?, ?, 'room_archived', ?, ?)`,
		value.id, value.revision, actorID, details, now); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	value.archived = true
	value.archiveBlob = blob
	value.snapshot = protocol.Snapshot{}
	value.tombstones = make(map[string]protocol.Line)
	value.sessions = make(map[string]*member)
	value.updatedAt, _ = time.Parse(time.RFC3339Nano, now)
	return nil
}

func (store *sqliteStore) unarchiveRoom(ctx context.Context, value *room, actorID string) error {
	if !value.archived {
		return nil
	}
	payload, err := decodeArchive(value.archiveBlob)
	if err != nil {
		return fmt.Errorf("decode room archive: %w", err)
	}
	snapshot, err := json.Marshal(payload.Snapshot)
	if err != nil {
		return err
	}
	tx, err := store.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if _, err := tx.ExecContext(ctx, `UPDATE rooms SET snapshot_json = ?, archived_at = NULL, archive_blob = NULL, updated_at = ? WHERE id = ?`, snapshot, now, value.id); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM tombstones WHERE room_id = ?`, value.id); err != nil {
		return err
	}
	for lineID, line := range payload.Tombstones {
		encoded, err := json.Marshal(line)
		if err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO tombstones (room_id, line_id, line_json, deleted_revision) VALUES (?, ?, ?, ?)`, value.id, lineID, encoded, value.revision); err != nil {
			return err
		}
	}
	details, _ := json.Marshal(map[string]any{"revision": value.revision})
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO audit_log (room_id, room_revision, actor_id, event_type, details_json, created_at) VALUES (?, ?, ?, 'room_unarchived', ?, ?)`,
		value.id, value.revision, actorID, details, now); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	value.archived = false
	value.archiveBlob = nil
	value.snapshot = cloneSnapshot(payload.Snapshot)
	value.tombstones = payload.Tombstones
	value.updatedAt, _ = time.Parse(time.RFC3339Nano, now)
	return nil
}

func (hub *hub) archiveInactive(ctx context.Context, now time.Time, after time.Duration) error {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	for _, value := range hub.rooms {
		if value.archived || len(value.members) != 0 || value.updatedAt.IsZero() || now.Sub(value.updatedAt) < after {
			continue
		}
		if err := hub.store.archiveRoom(ctx, value, "system"); err != nil {
			return err
		}
	}
	return nil
}

func BackupDatabase(ctx context.Context, databasePath, destination string) error {
	if databasePath == "" || destination == "" {
		return errors.New("database and destination paths are required")
	}
	absSource, err := filepath.Abs(databasePath)
	if err != nil {
		return err
	}
	absDestination, err := filepath.Abs(destination)
	if err != nil {
		return err
	}
	if absSource == absDestination {
		return errors.New("backup destination must differ from database")
	}
	if info, err := os.Stat(absSource); err != nil {
		return fmt.Errorf("database does not exist: %w", err)
	} else if info.IsDir() {
		return errors.New("database path is a directory")
	}
	if _, err := os.Stat(absDestination); err == nil {
		return errors.New("backup destination already exists")
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	store, err := openStore(absSource)
	if err != nil {
		return err
	}
	defer store.close()
	if _, err := store.db.ExecContext(ctx, `PRAGMA wal_checkpoint(PASSIVE)`); err != nil {
		return err
	}
	if _, err := store.db.ExecContext(ctx, `VACUUM INTO ?`, absDestination); err != nil {
		return fmt.Errorf("create SQLite backup: %w", err)
	}
	return nil
}

func RoomStats(ctx context.Context, databasePath string) ([]RoomStat, error) {
	store, err := openExistingStore(databasePath)
	if err != nil {
		return nil, err
	}
	defer store.close()
	rooms, err := store.loadRooms(ctx)
	if err != nil {
		return nil, err
	}
	stats := make([]RoomStat, 0, len(rooms))
	for _, value := range rooms {
		stat := RoomStat{Name: value.name, Revision: value.revision, Archived: value.archived, UpdatedAt: value.updatedAt.UTC().Format(time.RFC3339Nano), ArchiveBytes: int64(len(value.archiveBlob))}
		if !value.archived {
			stat.LiveLines = len(value.snapshot.Lines)
			stat.Comments = len(value.snapshot.Comments)
		} else if payload, err := decodeArchive(value.archiveBlob); err != nil {
			return nil, fmt.Errorf("decode archive for room %s: %w", value.name, err)
		} else {
			stat.LiveLines = len(payload.Snapshot.Lines)
			stat.Comments = len(payload.Snapshot.Comments)
		}
		if err := store.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM batches WHERE room_id = ?`, value.id).Scan(&stat.BatchRecords); err != nil {
			return nil, err
		}
		if err := store.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM audit_log WHERE room_id = ?`, value.id).Scan(&stat.AuditRecords); err != nil {
			return nil, err
		}
		stats = append(stats, stat)
	}
	return stats, nil
}

func SetRoomArchived(ctx context.Context, databasePath, roomName string, archived bool) error {
	store, err := openExistingStore(databasePath)
	if err != nil {
		return err
	}
	defer store.close()
	rooms, err := store.loadRooms(ctx)
	if err != nil {
		return err
	}
	for _, value := range rooms {
		if value.name != roomName {
			continue
		}
		if archived {
			return store.archiveRoom(ctx, value, "admin")
		}
		return store.unarchiveRoom(ctx, value, "admin")
	}
	return errRoomNotFound
}

func openExistingStore(databasePath string) (*sqliteStore, error) {
	if databasePath == "" {
		return nil, errors.New("database path is required")
	}
	info, err := os.Stat(databasePath)
	if err != nil {
		return nil, err
	}
	if info.IsDir() {
		return nil, errors.New("database path is a directory")
	}
	return openStore(databasePath)
}
