package collab

import (
	"testing"

	"github.com/coder/websocket"
)

func TestConnectedMembersSnapshotsConnection(t *testing.T) {
	connection := &websocket.Conn{}
	joined := &member{nickname: "alice", connection: connection}
	value := &room{members: map[string]*member{"alice": joined}}

	recipients := connectedMembers(value)
	if len(recipients) != 1 {
		t.Fatalf("connectedMembers returned %d recipients, want 1", len(recipients))
	}
	if recipients[0] == joined {
		t.Fatal("connectedMembers returned mutable room member instead of a snapshot")
	}

	joined.connection = nil
	if recipients[0].connection != connection {
		t.Fatal("recipient connection changed after the room member disconnected")
	}
}
