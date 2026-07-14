# Aegisub Together Protocol v1

Protocol v1 is a JSON message protocol carried by WebSocket at `/v1/ws`.
The server orders all persistent room events with a monotonically increasing
`room_revision`. One accepted subtitle batch consumes exactly one revision and
is applied atomically.

## Framing

- Control messages and payloads up to 32 KiB are UTF-8 JSON text frames.
- Larger batch and snapshot envelopes are binary frames. The first byte is
  `0x01`; the remaining bytes are a zlib stream containing the UTF-8 JSON
  envelope.
- A decompressed envelope may not exceed 64 MiB.
- Every envelope contains `protocol_version`, `type`, `request_id`,
  `room_revision`, and `payload`.
- Clients ignore unknown payload fields but reject an unknown message `type`.

## Connection sequence

1. The first application message is `access_auth`. It must arrive within five
   seconds. The password is an empty string when access authentication is not
   configured.
2. The server returns `access_ok`, after which the client sends `create_room`
   or `join_room`.
3. A successful create/join returns `room_joined` with the authoritative
   snapshot and current room revision.
4. Persistent events are broadcast as `batch_applied`, `comment_changed`, or
   `reindex`. Transient lock, presence, heartbeat, and maintenance messages do
   not increment the room revision unless explicitly documented otherwise.

## Ordering and conflict rules

- Dialogue identity is `line_id`; array indexes are never identities.
- `insert` and `move` name the client's known `left_id` and `right_id`. The
  server assigns the canonical `pos_key` and returns it in `batch_applied`.
- `modify`, `delete`, and `move` carry the line version observed by the client.
- Each entry in `batch_applied.operations` contains the normalized input
  `operation` and, when a line remains live, its complete canonical `line`.
  Section replacements return their new section version in the same entry.
- If allocating a position required a room-wide reindex, `batch_applied`
  includes `positions`, a complete line-ID-to-position-key map for the final
  post-batch document. Recipients apply operations and this map atomically.
- Styles and Script Info are whole-section values with independent versions.
- A failed validation rejects the complete batch with `batch_rejected`.
- The origin client advances its confirmed shadow only from `batch_applied`,
  never when it sends `submit_batch`.

## Maintenance arbitration

- `maintenance_request` grants the first requester an exclusive room-wide
  write lease and releases all line locks. Other members remain connected but
  their batches and lock requests are rejected with `maintenance_active`.
- The holder sends `maintenance_release` to finish normally. A non-holder may
  send `maintenance_cancel_request`, then `maintenance_cancel_force` after the
  server-advertised 30-second grace period.
- Only a successfully persisted holder batch renews the 10-minute idle lease.
  Heartbeats do not renew it, and the 60-minute hard limit never moves.
- Holder disconnect, idle expiry, hard expiry, release, and forced cancellation
  broadcast an inactive `maintenance_state` without incrementing the room
  revision.

`schema.json` is the normative wire schema. `errors.json` is the stable error
code registry. Files in `fixtures/` are canonical cross-language examples.
