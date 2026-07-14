package collab

import (
	"bytes"
	"compress/zlib"
	"encoding/json"
	"fmt"
	"testing"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
	"github.com/coder/websocket"
)

func TestWebSocketFrameCompressionRoundTrip(t *testing.T) {
	input := bytes.Repeat([]byte("subtitle data "), 4096)
	messageType, frame, err := encodeWebSocketFrame(input)
	if err != nil {
		t.Fatal(err)
	}
	if messageType != websocket.MessageBinary || len(frame) == 0 || frame[0] != binaryFrameZlib {
		t.Fatalf("large envelope was not encoded as zlib binary: type=%v size=%d", messageType, len(frame))
	}
	output, err := decodeWebSocketFrame(messageType, frame)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(input, output) {
		t.Fatal("compressed envelope did not round trip")
	}
}

func TestWebSocketFrameRejectsOversizedDecompression(t *testing.T) {
	var frame bytes.Buffer
	frame.WriteByte(binaryFrameZlib)
	compressor := zlib.NewWriter(&frame)
	_, _ = compressor.Write(bytes.Repeat([]byte{'x'}, 1025))
	if err := compressor.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := decodeWebSocketFrameWithLimit(websocket.MessageBinary, frame.Bytes(), 1024); err == nil {
		t.Fatal("expected decompression limit rejection")
	}
}

func TestLargeSnapshotUsesCompressedFramesInBothDirections(t *testing.T) {
	_, url := testServer(t, "")
	creator := dial(t, url)
	authenticate(t, creator, "")
	snapshot := sampleSnapshot()
	snapshot.Lines = make([]protocol.Line, 200)
	for index := range snapshot.Lines {
		line := sampleSnapshot().Lines[0]
		line.LineID = fmt.Sprintf("0000000001-%d", index+1)
		line.PosKey = fmt.Sprintf("%08d", index)
		text := string(bytes.Repeat([]byte{'x'}, 512))
		line.Fields.Text = &text
		snapshot.Lines[index] = line
	}
	send(t, creator, "create_room", "large-create", protocol.CreateRoom{
		RoomName: "large-room", RoomPassword: "room password", Nickname: "creator", LockEnabled: true, Snapshot: snapshot,
	})
	createdEnvelope, createdFrameType := receiveFrame(t, creator)
	if createdEnvelope.Type != "room_joined" {
		t.Fatalf("expected room_joined, got %s", createdEnvelope.Type)
	}
	if createdFrameType != websocket.MessageBinary {
		t.Fatalf("expected compressed room snapshot, got frame type %v", createdFrameType)
	}

	joiner := dial(t, url)
	authenticate(t, joiner, "")
	send(t, joiner, "join_room", "large-join", protocol.JoinRoom{RoomName: "large-room", RoomPassword: "room password", Nickname: "joiner"})
	joinedEnvelope, joinedFrameType := receiveFrame(t, joiner)
	if joinedFrameType != websocket.MessageBinary {
		t.Fatalf("expected compressed joined snapshot, got frame type %v", joinedFrameType)
	}
	var joined protocol.RoomJoined
	if err := json.Unmarshal(joinedEnvelope.Payload, &joined); err != nil {
		t.Fatal(err)
	}
	if len(joined.Snapshot.Lines) != len(snapshot.Lines) || *joined.Snapshot.Lines[199].Fields.Text != *snapshot.Lines[199].Fields.Text {
		t.Fatal("large compressed snapshot changed during round trip")
	}
}
