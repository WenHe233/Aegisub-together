package protocol

import "encoding/json"

const Version = 2

const MaximumLockSetSize = 10000

type Envelope struct {
	ProtocolVersion int             `json:"protocol_version"`
	Type            string          `json:"type"`
	RequestID       string          `json:"request_id"`
	RoomRevision    int64           `json:"room_revision"`
	Payload         json.RawMessage `json:"payload"`
}

type OutboundEnvelope struct {
	ProtocolVersion int    `json:"protocol_version"`
	Type            string `json:"type"`
	RequestID       string `json:"request_id"`
	RoomRevision    int64  `json:"room_revision"`
	Payload         any    `json:"payload"`
}

type AccessAuth struct {
	Password string `json:"password"`
}

type CreateRoom struct {
	RoomName     string   `json:"room_name"`
	RoomPassword string   `json:"room_password"`
	Nickname     string   `json:"nickname"`
	LockEnabled  bool     `json:"lock_enabled"`
	Snapshot     Snapshot `json:"snapshot"`
}

type JoinRoom struct {
	RoomName     string `json:"room_name"`
	RoomPassword string `json:"room_password"`
	Nickname     string `json:"nickname"`
	ResumeToken  string `json:"resume_token,omitempty"`
}

type Snapshot struct {
	Lines             []Line            `json:"lines"`
	Styles            []string          `json:"styles"`
	StylesVersion     int64             `json:"styles_version"`
	ScriptInfo        []ScriptInfoEntry `json:"script_info"`
	ScriptInfoVersion int64             `json:"script_info_version"`
	Comments          []Comment         `json:"comments"`
}

type Line struct {
	LineID  string     `json:"line_id"`
	PosKey  string     `json:"pos_key"`
	Version int64      `json:"version"`
	Fields  LineFields `json:"fields"`
}

type LineFields struct {
	Comment *bool   `json:"comment,omitempty"`
	Layer   *int    `json:"layer,omitempty"`
	StartMS *int64  `json:"start_ms,omitempty"`
	EndMS   *int64  `json:"end_ms,omitempty"`
	Style   *string `json:"style,omitempty"`
	Actor   *string `json:"actor,omitempty"`
	Effect  *string `json:"effect,omitempty"`
	Margins []int   `json:"margins,omitempty"`
	Text    *string `json:"text,omitempty"`
}

type ScriptInfoEntry struct {
	Key   string `json:"key"`
	Value string `json:"value"`
}

type Comment struct {
	CommentID       string  `json:"comment_id"`
	LineID          string  `json:"line_id"`
	AuthorID        string  `json:"author_id"`
	AuthorName      string  `json:"author_name"`
	Body            string  `json:"body"`
	SuggestedText   *string `json:"suggested_text,omitempty"`
	BaseLineVersion int64   `json:"base_line_version"`
	State           string  `json:"state"`
	CreatedAt       string  `json:"created_at"`
	ResolvedBy      *string `json:"resolved_by,omitempty"`
}

type CommentCreate struct {
	LineID          string  `json:"line_id"`
	BaseLineVersion int64   `json:"base_line_version"`
	Body            string  `json:"body"`
	SuggestedText   *string `json:"suggested_text"`
}

type CommentSetState struct {
	CommentID string `json:"comment_id"`
	State     string `json:"state"`
}

type CommentChanged struct {
	Comment Comment `json:"comment"`
	Line    *Line   `json:"line"`
	ActorID string  `json:"actor_id"`
}

type RoomJoined struct {
	RoomID      string         `json:"room_id"`
	MemberID    string         `json:"member_id"`
	ResumeToken string         `json:"resume_token"`
	LockEnabled bool           `json:"lock_enabled"`
	Snapshot    Snapshot       `json:"snapshot"`
	LockSets    []LockSetState `json:"lock_sets"`
	Presence    Presence       `json:"presence"`
}

type SnapshotRequest struct {
	AfterRevision int64 `json:"after_revision"`
}

type SnapshotState struct {
	Revision int64    `json:"revision"`
	Snapshot Snapshot `json:"snapshot"`
}

type AuditRequest struct {
	AfterID int64 `json:"after_id"`
	Limit   int   `json:"limit"`
}

type AuditEntry struct {
	ID           int64           `json:"id"`
	RoomRevision int64           `json:"room_revision"`
	ActorID      string          `json:"actor_id"`
	EventType    string          `json:"event_type"`
	Details      json.RawMessage `json:"details"`
	CreatedAt    string          `json:"created_at"`
}

type AuditPage struct {
	Entries     []AuditEntry `json:"entries"`
	NextAfterID int64        `json:"next_after_id"`
}

type SubmitBatch struct {
	BatchID    string            `json:"batch_id"`
	Operations []json.RawMessage `json:"operations"`
}

type AppliedOperation struct {
	Operation         json.RawMessage `json:"operation"`
	Line              *Line           `json:"line,omitempty"`
	StylesVersion     int64           `json:"styles_version,omitempty"`
	ScriptInfoVersion int64           `json:"script_info_version,omitempty"`
}

type BatchApplied struct {
	BatchID    string             `json:"batch_id"`
	ActorID    string             `json:"actor_id"`
	Operations []AppliedOperation `json:"operations"`
	IDRemap    map[string]string  `json:"id_remap"`
	Positions  map[string]string  `json:"positions,omitempty"`
}

type BatchRejected struct {
	BatchID        string `json:"batch_id"`
	Code           string `json:"code"`
	Message        string `json:"message"`
	LineID         string `json:"line_id,omitempty"`
	OperationIndex int    `json:"operation_index,omitempty"`
}

type LineReference struct {
	LineID string `json:"line_id"`
}

type LockSetRequest struct {
	LineIDs      []string `json:"line_ids"`
	ActiveLineID *string  `json:"active_line_id"`
	Generation   int64    `json:"generation"`
}

type LockConflict struct {
	LineID      string `json:"line_id"`
	HolderID    string `json:"holder_id"`
	HolderName  string `json:"holder_name"`
	ExpiresInMS int64  `json:"expires_in_ms"`
}

type LockSetState struct {
	MemberID   string         `json:"member_id"`
	MemberName string         `json:"member_name"`
	Granted    bool           `json:"granted"`
	LineIDs    []string       `json:"line_ids"`
	Conflicts  []LockConflict `json:"conflicts"`
	Generation int64          `json:"generation"`
}

type LockState struct {
	LineID      string  `json:"line_id"`
	RequesterID string  `json:"requester_id"`
	Granted     bool    `json:"granted"`
	HolderID    *string `json:"holder_id"`
	HolderName  *string `json:"holder_name"`
	ExpiresInMS int64   `json:"expires_in_ms"`
}

type PresenceMember struct {
	MemberID string  `json:"member_id"`
	Nickname string  `json:"nickname"`
	LineID   *string `json:"line_id"`
	LastSeen string  `json:"last_seen"`
}

type Presence struct {
	Members []PresenceMember `json:"members"`
}

type MaintenanceState struct {
	Active              bool    `json:"active"`
	HolderID            *string `json:"holder_id"`
	HolderName          *string `json:"holder_name"`
	StartedAt           *string `json:"started_at"`
	IdleExpiresAt       *string `json:"idle_expires_at"`
	HardExpiresAt       *string `json:"hard_expires_at"`
	CancelRequestedBy   *string `json:"cancel_requested_by"`
	CancelRequestedName *string `json:"cancel_requested_name"`
	CancelForceAt       *string `json:"cancel_force_at"`
}

type ModifyOperation struct {
	Op          string     `json:"op"`
	LineID      string     `json:"line_id"`
	BaseVersion int64      `json:"base_version"`
	Fields      LineFields `json:"fields"`
}

type InsertOperation struct {
	Op      string     `json:"op"`
	LineID  string     `json:"line_id"`
	LeftID  *string    `json:"left_id"`
	RightID *string    `json:"right_id"`
	Fields  LineFields `json:"fields"`
}

type DeleteOperation struct {
	Op          string `json:"op"`
	LineID      string `json:"line_id"`
	BaseVersion int64  `json:"base_version"`
}

type MoveOperation struct {
	Op          string  `json:"op"`
	LineID      string  `json:"line_id"`
	LeftID      *string `json:"left_id"`
	RightID     *string `json:"right_id"`
	BaseVersion int64   `json:"base_version"`
}

type RestoreOperation struct {
	Op     string `json:"op"`
	LineID string `json:"line_id"`
}

type ReplaceStylesOperation struct {
	Op          string   `json:"op"`
	BaseVersion int64    `json:"base_version"`
	Styles      []string `json:"styles"`
}

type ReplaceScriptInfoOperation struct {
	Op          string            `json:"op"`
	BaseVersion int64             `json:"base_version"`
	Entries     []ScriptInfoEntry `json:"entries"`
}

type Error struct {
	Code         string `json:"code"`
	Message      string `json:"message"`
	Retryable    bool   `json:"retryable"`
	RetryAfterMS int64  `json:"retry_after_ms,omitempty"`
}
