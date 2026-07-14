# Aegisub Together 服务端

生产环境的 Docker Compose/Caddy 和 systemd 示例参见[协作服务端部署指南](../docs/collaboration-deployment.md)。

使用 Go 1.26.5 构建服务端：

```text
go build ./cmd/aegisub-together-server
```

该二进制只提供本机管理命令，不提供可列出房间的 HTTP API。

```text
aegisub-together-server hash-password < password.txt
aegisub-together-server serve -listen :8080 -database collab.db \
  -access-password-hash '<argon2id-hash>' -archive-days 0
aegisub-together-server backup -database collab.db backup.db
aegisub-together-server rooms stats -database collab.db
aegisub-together-server rooms archive -database collab.db room-name
aegisub-together-server rooms unarchive -database collab.db room-name
```

`hash-password` 从标准输入读取密码，避免明文出现在进程列表中。`serve` 也接受以下环境变量：

- `AEGISUB_COLLAB_LISTEN`
- `AEGISUB_COLLAB_DATABASE`
- `AEGISUB_COLLAB_ACCESS_PASSWORD_HASH`
- `AEGISUB_COLLAB_TRUSTED_PROXY_CIDRS`

`AEGISUB_COLLAB_TRUSTED_PROXY_CIDRS` 是以逗号分隔的可信代理网段白名单。只有直连来源属于这些网段时，服务端才会采用转发的客户端地址，否则忽略 `X-Forwarded-For` 等转发头。

`archive-days` 默认为 `0`，即关闭自动归档。冷归档会压缩权威快照、批注和墓碑，并裁剪回放批次；房名、密码哈希和 revision 会继续保留。客户端使用原凭据再次加入时会自动恢复房间。归档绝不会永久删除字幕数据。

`backup` 使用 SQLite 的在线 `VACUUM INTO` 路径，并拒绝覆盖已有目标。备份应存放在实时数据库目录之外，并定期验证它能正常打开和恢复房间。

服务端自身提供 HTTP/WebSocket。公网部署应在前方使用 Caddy，通过 `wss://` 提供 TLS；直接 `ws://` 只适合本机或受防火墙保护的可信局域网，因为密码、字幕和协作数据都会明文传输。
