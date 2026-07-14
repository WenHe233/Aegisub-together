# Deploying the collaboration server

The collaboration endpoint must be exposed as `wss://HOST/v1/ws`. The server
itself speaks HTTP on a private interface; Caddy terminates TLS, validates and
renews the public certificate, and proxies WebSocket connections. Only ports
80 and 443 should be reachable from the Internet.

The server never needs the room passwords: clients provide them while creating
rooms and the database stores Argon2id hashes. The optional server access
password is also configured as an Argon2id hash. Plaintext passwords must not
be placed in Compose files, systemd units, shell history, or logs.

## Docker Compose and Caddy

Copy [`deploy`](../deploy) to the host, then create the local configuration:

```sh
cd deploy
cp .env.example .env
cp server.env.example server.env
chmod 600 .env server.env
```

Set `COLLAB_DOMAIN` to a DNS name whose A/AAAA record points to the host. Pin
`SERVER_IMAGE` to a released `vX.Y.Z` image in production. `latest` is useful
for evaluation and `edge` tracks the default branch, but neither is an
immutable production reference.

Generate the access-password hash without placing the plaintext in argv:

```sh
printf '%s\n' 'replace with a strong password' | \
  docker run --rm -i ghcr.io/wenhe233/aegisub-together-server:v0.1.0 hash-password
```

Put the resulting value in `server.env` inside single quotes. Set
`ARCHIVE_DAYS=0` to disable cold archival, or a positive inactivity period.
Archival compresses room state and trims replay logs; it never permanently
deletes subtitles and valid room credentials automatically restore the room.

Validate and start the stack:

```sh
docker compose config --quiet
docker compose pull
docker compose up -d
curl --fail https://collab.example.com/healthz
```

The Compose network gives Caddy the fixed address `172.28.0.2`. The server
trusts forwarded addresses only from that exact `/32`; requests from any other
peer cannot spoof `X-Forwarded-For` to evade authentication rate limits. If the
network layout changes, update the address and trusted CIDR together. Do not
publish the server container's port 8080.

### Back up and restore the Compose volume

SQLite remains online while the administration command creates a consistent
backup. Use a unique destination name because the command refuses to overwrite
files:

```sh
stamp=$(date -u +%Y%m%dT%H%M%SZ)
docker compose exec -T server /usr/local/bin/aegisub-together-server \
  backup -database /data/collab.db "/data/backup-$stamp.db"
docker compose cp "server:/data/backup-$stamp.db" "./backup-$stamp.db"
docker compose run --rm --no-deps server \
  rooms stats -database "/data/backup-$stamp.db"
```

Copy backups off the Docker host and periodically test them. To restore, stop
the server, retain a copy of the current database and its `-wal`/`-shm` files,
replace `collab.db` in the named volume with a verified backup, set ownership
to UID/GID `65532`, and start the server. Never copy a live SQLite database file
directly; use the `backup` command.

## Native binary with systemd

Install the released binary and create a dedicated account:

```sh
sha256sum -c SHA256SUMS
sudo install -m 0755 aegisub-together-server-v0.1.0-linux-amd64 \
  /usr/local/bin/aegisub-together-server
sudo useradd --system --home /var/lib/aegisub-together \
  --shell /usr/sbin/nologin aegisub-collab
sudo install -d -m 0750 /etc/aegisub-together
sudo install -m 0644 deploy/systemd/aegisub-together-server.service \
  /etc/systemd/system/aegisub-together-server.service
sudo install -m 0600 deploy/systemd/server.env.example \
  /etc/aegisub-together/server.env
```

Generate a hash with `/usr/local/bin/aegisub-together-server hash-password`,
edit `server.env`, then install Caddy and copy `deploy/systemd/Caddyfile` to
`/etc/caddy/Caddyfile` after replacing the example domain. The server binds
only to loopback and trusts only loopback proxies.

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now aegisub-together-server caddy
sudo systemctl status aegisub-together-server caddy
curl --fail https://collab.example.com/healthz
```

The unit uses a private temporary directory, a read-only operating-system
filesystem, an empty capability set, syscall and address-family restrictions,
and a systemd-managed state directory. Check hardening on the target distro:

```sh
systemd-analyze security aegisub-together-server.service
```

For an online native backup:

```sh
sudo install -d -o aegisub-collab -g aegisub-collab -m 0700 \
  /var/backups/aegisub-together
sudo -u aegisub-collab /usr/local/bin/aegisub-together-server backup \
  -database /var/lib/aegisub-together/collab.db \
  "/var/backups/aegisub-together/collab-$(date -u +%Y%m%dT%H%M%SZ).db"
sudo -u aegisub-collab /usr/local/bin/aegisub-together-server rooms stats \
  -database /var/backups/aegisub-together/VERIFIED-BACKUP.db
```

Restore only while the service is stopped. Keep the displaced database and
WAL files until `rooms stats`, service startup, room join, and a fresh backup
all succeed.

## Operations and security checks

- Keep Caddy and the server image/binary patched; stage version upgrades and
  retain the previous immutable image and database backup for rollback.
- Verify `SHA256SUMS` before installing release files and retain the supplied
  SPDX JSON SBOM with deployment records.
- Monitor Caddy and service/container logs, but do not add request payloads,
  passwords, room snapshots, or full WebSocket frames to logs.
- A TLS certificate warning in Aegisub is a deployment failure. Do not bypass
  validation; repair DNS, time, the certificate chain, or Caddy instead.
- `/healthz` intentionally returns only `ok`. Room statistics are available
  only through the local `rooms stats` command.
- Test rate limiting through Caddy, database restore, a server restart with two
  clients, and WebSocket upgrades before inviting users.
- Leave `COLLABORATION_RELEASE_APPROVED` unset until the complete M1-M3 and
  real two-client acceptance suite has passed. Set it to `true` only when a
  `vX.Y.Z` tag is intended to become a public GitHub Release.
