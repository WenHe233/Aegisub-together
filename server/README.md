# Aegisub Together server

Build the server with Go 1.26.5:

```text
go build ./cmd/aegisub-together-server
```

The binary exposes local administration commands only; it does not provide a
room-listing HTTP API.

```text
aegisub-together-server hash-password < password.txt
aegisub-together-server serve -listen :8080 -database collab.db \
  -access-password-hash '<argon2id-hash>' -archive-days 0
aegisub-together-server backup -database collab.db backup.db
aegisub-together-server rooms stats -database collab.db
aegisub-together-server rooms archive -database collab.db room-name
aegisub-together-server rooms unarchive -database collab.db room-name
```

`hash-password` reads standard input so the plaintext secret does not appear in
the process list. `serve` also accepts `AEGISUB_COLLAB_LISTEN`,
`AEGISUB_COLLAB_DATABASE`, and `AEGISUB_COLLAB_ACCESS_PASSWORD_HASH`.

`archive-days` defaults to `0`, which disables automatic archival. A cold
archive compresses the authoritative snapshot, comments, and tombstones and
trims replay batches. It retains the room name, password hash, and revision.
The original room credentials restore it automatically on the next join.
Archival never permanently deletes subtitle data.

`backup` uses SQLite's online `VACUUM INTO` path and refuses to overwrite an
existing destination. Keep backups outside the live database directory and
periodically test that they open and restore rooms.
