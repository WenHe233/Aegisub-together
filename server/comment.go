package collab

import (
	"context"
	"errors"
	"time"
	"unicode/utf8"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
)

type commentFailure struct {
	code    string
	message string
}

func (failure *commentFailure) Error() string { return failure.message }

func (hub *hub) createComment(ctx context.Context, roomID, actorID string, input protocol.CommentCreate, now time.Time) (protocol.CommentChanged, int64, []*member, error) {
	if input.LineID == "" || input.BaseLineVersion < 1 || !validCommentBody(input.Body) || !validSuggestedText(input.SuggestedText) {
		return protocol.CommentChanged{}, 0, nil, &commentFailure{code: "invalid_message", message: "comment is invalid"}
	}
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	actor := memberByID(value, actorID)
	if value == nil || actor == nil {
		return protocol.CommentChanged{}, 0, nil, errors.New("room or member does not exist")
	}
	if value.maintenance != nil && value.maintenance.holderID != actorID {
		return protocol.CommentChanged{}, value.revision, nil, &commentFailure{code: "maintenance_active", message: "room is frozen for maintenance"}
	}
	line := liveLine(value.snapshot.Lines, input.LineID)
	if line == nil || line.Version != input.BaseLineVersion {
		return protocol.CommentChanged{}, value.revision, nil, &commentFailure{code: "comment_version_conflict", message: "comment is based on a stale line version"}
	}
	commentID, err := secureToken(16)
	if err != nil {
		return protocol.CommentChanged{}, value.revision, nil, err
	}
	working := cloneRoomState(value)
	comment := protocol.Comment{
		CommentID: "comment-" + commentID, LineID: input.LineID, AuthorID: actorID, AuthorName: actor.nickname,
		Body: input.Body, SuggestedText: input.SuggestedText, BaseLineVersion: line.Version, State: "open", CreatedAt: now.UTC().Format(time.RFC3339Nano),
	}
	working.snapshot.Comments = append(working.snapshot.Comments, comment)
	working.revision++
	changed := protocol.CommentChanged{Comment: comment, ActorID: actorID}
	if err := hub.store.saveCommentChange(ctx, working, changed, actorID, "comment_created"); err != nil {
		return protocol.CommentChanged{}, value.revision, nil, err
	}
	value.snapshot = working.snapshot
	value.revision = working.revision
	value.updatedAt = now
	return changed, value.revision, connectedMembers(value), nil
}

func (hub *hub) setCommentState(ctx context.Context, roomID, actorID string, input protocol.CommentSetState, now time.Time) (protocol.CommentChanged, int64, []*member, error) {
	if input.CommentID == "" || (input.State != "accepted" && input.State != "rejected" && input.State != "resolved") {
		return protocol.CommentChanged{}, 0, nil, &commentFailure{code: "invalid_message", message: "comment state request is invalid"}
	}
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	if value == nil || memberByID(value, actorID) == nil {
		return protocol.CommentChanged{}, 0, nil, errors.New("room or member does not exist")
	}
	if value.maintenance != nil && value.maintenance.holderID != actorID {
		return protocol.CommentChanged{}, value.revision, nil, &commentFailure{code: "maintenance_active", message: "room is frozen for maintenance"}
	}
	commentIndex := findComment(value.snapshot.Comments, input.CommentID)
	if commentIndex < 0 || value.snapshot.Comments[commentIndex].State != "open" {
		return protocol.CommentChanged{}, value.revision, nil, &commentFailure{code: "comment_version_conflict", message: "comment is missing or already closed"}
	}
	working := cloneRoomState(value)
	comment := &working.snapshot.Comments[commentIndex]
	var changedLine *protocol.Line
	if input.State == "accepted" {
		if comment.SuggestedText == nil {
			return protocol.CommentChanged{}, value.revision, nil, &commentFailure{code: "comment_version_conflict", message: "comment has no suggested text"}
		}
		line, lineIndex := findLine(working.snapshot.Lines, comment.LineID)
		if line == nil || line.Version != comment.BaseLineVersion {
			return protocol.CommentChanged{}, value.revision, nil, &commentFailure{code: "comment_version_conflict", message: "suggestion is based on a stale or deleted line"}
		}
		if value.lockEnabled {
			lock, held := value.locks[comment.LineID]
			if !held || lock.memberID != actorID {
				return protocol.CommentChanged{}, value.revision, nil, &commentFailure{code: "line_locked", message: "accepting a suggestion requires the line lock"}
			}
		}
		text := *comment.SuggestedText
		line.Fields.Text = &text
		line.Version++
		working.snapshot.Lines[lineIndex] = *line
		changedLine = line
	}
	comment.State = input.State
	comment.ResolvedBy = stringPointerValue(actorID)
	working.revision++
	changed := protocol.CommentChanged{Comment: *comment, Line: changedLine, ActorID: actorID}
	if err := hub.store.saveCommentChange(ctx, working, changed, actorID, "comment_"+input.State); err != nil {
		return protocol.CommentChanged{}, value.revision, nil, err
	}
	value.snapshot = working.snapshot
	value.revision = working.revision
	value.updatedAt = now
	if input.State == "accepted" {
		if lock, held := value.locks[comment.LineID]; held && lock.memberID == actorID {
			lock.lastActivity = now
			value.locks[comment.LineID] = lock
		}
	}
	return changed, value.revision, connectedMembers(value), nil
}

func (hub *hub) snapshotState(roomID string) (protocol.SnapshotState, bool) {
	hub.mu.Lock()
	defer hub.mu.Unlock()
	value := hub.roomByID(roomID)
	if value == nil {
		return protocol.SnapshotState{}, false
	}
	return protocol.SnapshotState{Revision: value.revision, Snapshot: cloneSnapshot(value.snapshot)}, true
}

func findComment(comments []protocol.Comment, commentID string) int {
	for index := range comments {
		if comments[index].CommentID == commentID {
			return index
		}
	}
	return -1
}

func validCommentBody(body string) bool {
	return body != "" && utf8.ValidString(body) && utf8.RuneCountInString(body) <= 16384
}

func validSuggestedText(text *string) bool {
	return text == nil || (utf8.ValidString(*text) && utf8.RuneCountInString(*text) <= 1<<20)
}
