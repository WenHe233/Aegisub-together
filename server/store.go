package collab

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
	_ "modernc.org/sqlite"
)

type sqliteStore struct {
	db *sql.DB
}

func openStore(path string) (*sqliteStore, error) {
	if path == "" {
		path = ":memory:"
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, fmt.Errorf("open SQLite database: %w", err)
	}
	db.SetMaxOpenConns(1)
	store := &sqliteStore{db: db}
	if err := store.initialize(context.Background()); err != nil {
		db.Close()
		return nil, err
	}
	return store, nil
}

func (store *sqliteStore) initialize(ctx context.Context) error {
	statements := []string{
		`PRAGMA journal_mode = WAL`,
		`PRAGMA foreign_keys = ON`,
		`PRAGMA busy_timeout = 5000`,
		`CREATE TABLE IF NOT EXISTS rooms (
			id TEXT PRIMARY KEY,
			name TEXT NOT NULL UNIQUE,
			password_hash TEXT NOT NULL,
			lock_enabled INTEGER NOT NULL,
			revision INTEGER NOT NULL,
			snapshot_json BLOB NOT NULL,
			created_at TEXT NOT NULL,
			updated_at TEXT NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS tombstones (
			room_id TEXT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
			line_id TEXT NOT NULL,
			line_json BLOB NOT NULL,
			deleted_revision INTEGER NOT NULL,
			PRIMARY KEY (room_id, line_id)
		)`,
		`CREATE TABLE IF NOT EXISTS batches (
			room_id TEXT NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
			batch_id TEXT NOT NULL,
			revision INTEGER NOT NULL,
			actor_id TEXT NOT NULL,
			result_json BLOB NOT NULL,
			created_at TEXT NOT NULL,
			PRIMARY KEY (room_id, batch_id),
			UNIQUE (room_id, revision)
		)`,
		`CREATE TABLE IF NOT EXISTS audit_log (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			room_id TEXT,
			actor_id TEXT,
			event_type TEXT NOT NULL,
			details_json BLOB NOT NULL,
			created_at TEXT NOT NULL
		)`,
	}
	for _, statement := range statements {
		if _, err := store.db.ExecContext(ctx, statement); err != nil {
			return fmt.Errorf("initialize SQLite database: %w", err)
		}
	}
	return nil
}

func (store *sqliteStore) close() error {
	return store.db.Close()
}

func (store *sqliteStore) createRoom(ctx context.Context, value *room) error {
	snapshot, err := json.Marshal(value.snapshot)
	if err != nil {
		return err
	}
	now := time.Now().UTC().Format(time.RFC3339Nano)
	_, err = store.db.ExecContext(ctx,
		`INSERT INTO rooms (id, name, password_hash, lock_enabled, revision, snapshot_json, created_at, updated_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		value.id, value.name, value.passwordHash, value.lockEnabled, value.revision, snapshot, now, now,
	)
	if err != nil {
		return fmt.Errorf("persist room: %w", err)
	}
	return nil
}

func (store *sqliteStore) loadRooms(ctx context.Context) ([]*room, error) {
	rows, err := store.db.QueryContext(ctx, `SELECT id, name, password_hash, lock_enabled, revision, snapshot_json FROM rooms ORDER BY name`)
	if err != nil {
		return nil, fmt.Errorf("load rooms: %w", err)
	}
	defer rows.Close()
	var rooms []*room
	for rows.Next() {
		value := &room{members: make(map[string]*member), tombstones: make(map[string]protocol.Line), locks: make(map[string]lineLock)}
		var lockEnabled int
		var snapshot []byte
		if err := rows.Scan(&value.id, &value.name, &value.passwordHash, &lockEnabled, &value.revision, &snapshot); err != nil {
			return nil, err
		}
		value.lockEnabled = lockEnabled != 0
		if err := json.Unmarshal(snapshot, &value.snapshot); err != nil {
			return nil, fmt.Errorf("decode room %s snapshot: %w", value.name, err)
		}
		rooms = append(rooms, value)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	for _, value := range rooms {
		if err := store.loadTombstones(ctx, value); err != nil {
			return nil, err
		}
	}
	return rooms, nil
}

func (store *sqliteStore) loadTombstones(ctx context.Context, value *room) error {
	rows, err := store.db.QueryContext(ctx, `SELECT line_id, line_json FROM tombstones WHERE room_id = ?`, value.id)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var lineID string
		var encoded []byte
		if err := rows.Scan(&lineID, &encoded); err != nil {
			return err
		}
		var line protocol.Line
		if err := json.Unmarshal(encoded, &line); err != nil {
			return err
		}
		value.tombstones[lineID] = line
	}
	return rows.Err()
}

func (store *sqliteStore) saveBatch(ctx context.Context, value *room, result protocol.BatchApplied, actorID string) error {
	snapshot, err := json.Marshal(value.snapshot)
	if err != nil {
		return err
	}
	resultJSON, err := json.Marshal(result)
	if err != nil {
		return err
	}
	tx, err := store.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if _, err := tx.ExecContext(ctx, `UPDATE rooms SET revision = ?, snapshot_json = ?, updated_at = ? WHERE id = ?`, value.revision, snapshot, now, value.id); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM tombstones WHERE room_id = ?`, value.id); err != nil {
		return err
	}
	for lineID, line := range value.tombstones {
		encoded, err := json.Marshal(line)
		if err != nil {
			return err
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO tombstones (room_id, line_id, line_json, deleted_revision) VALUES (?, ?, ?, ?)`, value.id, lineID, encoded, value.revision); err != nil {
			return err
		}
	}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO batches (room_id, batch_id, revision, actor_id, result_json, created_at) VALUES (?, ?, ?, ?, ?, ?)`,
		value.id, result.BatchID, value.revision, actorID, resultJSON, now,
	); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO audit_log (room_id, actor_id, event_type, details_json, created_at) VALUES (?, ?, 'batch_applied', ?, ?)`,
		value.id, actorID, resultJSON, now,
	); err != nil {
		return err
	}
	return tx.Commit()
}

func (store *sqliteStore) findBatch(ctx context.Context, roomID, batchID string) (protocol.BatchApplied, int64, bool, error) {
	var encoded []byte
	var revision int64
	err := store.db.QueryRowContext(ctx, `SELECT result_json, revision FROM batches WHERE room_id = ? AND batch_id = ?`, roomID, batchID).Scan(&encoded, &revision)
	if errors.Is(err, sql.ErrNoRows) {
		return protocol.BatchApplied{}, 0, false, nil
	}
	if err != nil {
		return protocol.BatchApplied{}, 0, false, err
	}
	var result protocol.BatchApplied
	if err := json.Unmarshal(encoded, &result); err != nil {
		return protocol.BatchApplied{}, 0, false, err
	}
	return result, revision, true, nil
}

func (store *sqliteStore) audit(ctx context.Context, roomID, actorID, eventType string, details any) error {
	encoded, err := json.Marshal(details)
	if err != nil {
		return err
	}
	_, err = store.db.ExecContext(ctx,
		`INSERT INTO audit_log (room_id, actor_id, event_type, details_json, created_at) VALUES (?, ?, ?, ?, ?)`,
		roomID, actorID, eventType, encoded, time.Now().UTC().Format(time.RFC3339Nano),
	)
	return err
}
