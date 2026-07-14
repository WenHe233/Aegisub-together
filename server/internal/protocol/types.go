package protocol

import "encoding/json"

const Version = 1

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

type RoomJoined struct {
	RoomID      string   `json:"room_id"`
	MemberID    string   `json:"member_id"`
	ResumeToken string   `json:"resume_token"`
	LockEnabled bool     `json:"lock_enabled"`
	Snapshot    Snapshot `json:"snapshot"`
}

type Error struct {
	Code         string `json:"code"`
	Message      string `json:"message"`
	Retryable    bool   `json:"retryable"`
	RetryAfterMS int64  `json:"retry_after_ms,omitempty"`
}
