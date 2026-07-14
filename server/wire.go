package collab

import (
	"bytes"
	"compress/zlib"
	"errors"
	"fmt"
	"io"
	"unicode/utf8"

	"github.com/coder/websocket"
)

const (
	textFrameLimit  = 32 << 10
	binaryFrameZlib = byte(0x01)
)

func encodeWebSocketFrame(data []byte) (websocket.MessageType, []byte, error) {
	if len(data) > maxMessageSize {
		return 0, nil, errors.New("collaboration envelope exceeds 64 MiB")
	}
	if len(data) <= textFrameLimit {
		return websocket.MessageText, data, nil
	}
	var output bytes.Buffer
	output.WriteByte(binaryFrameZlib)
	compressor := zlib.NewWriter(&output)
	if _, err := compressor.Write(data); err != nil {
		return 0, nil, fmt.Errorf("compress collaboration envelope: %w", err)
	}
	if err := compressor.Close(); err != nil {
		return 0, nil, fmt.Errorf("finish collaboration envelope compression: %w", err)
	}
	return websocket.MessageBinary, output.Bytes(), nil
}

func decodeWebSocketFrame(messageType websocket.MessageType, data []byte) ([]byte, error) {
	return decodeWebSocketFrameWithLimit(messageType, data, maxMessageSize)
}

func decodeWebSocketFrameWithLimit(messageType websocket.MessageType, data []byte, limit int64) ([]byte, error) {
	var decoded []byte
	switch messageType {
	case websocket.MessageText:
		if len(data) > textFrameLimit {
			return nil, errors.New("text collaboration envelope exceeds 32 KiB")
		}
		decoded = data
	case websocket.MessageBinary:
		if len(data) < 2 || data[0] != binaryFrameZlib {
			return nil, errors.New("unknown collaboration binary frame")
		}
		reader, err := zlib.NewReader(bytes.NewReader(data[1:]))
		if err != nil {
			return nil, fmt.Errorf("open collaboration envelope compression: %w", err)
		}
		decoded, err = io.ReadAll(io.LimitReader(reader, limit+1))
		closeErr := reader.Close()
		if err != nil {
			return nil, fmt.Errorf("decompress collaboration envelope: %w", err)
		}
		if closeErr != nil {
			return nil, fmt.Errorf("close collaboration envelope compression: %w", closeErr)
		}
		if int64(len(decoded)) > limit {
			return nil, errors.New("decompressed collaboration envelope exceeds 64 MiB")
		}
	default:
		return nil, errors.New("unsupported WebSocket message type")
	}
	if !utf8.Valid(decoded) {
		return nil, errors.New("collaboration envelope is not valid UTF-8")
	}
	return decoded, nil
}
