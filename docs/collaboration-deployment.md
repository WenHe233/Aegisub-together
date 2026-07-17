# 协作服务端部署指南

公网部署强烈建议将协作端点暴露为 `wss://HOST/v1/ws`。Go 服务端在私有接口上提供 HTTP/WebSocket，Caddy 负责终止 TLS、申请并续期公网证书，以及反向代理 WebSocket。公网只应开放 80 和 443 端口。

客户端也允许连接 `ws://HOST/v1/ws`，但 `ws://` 不加密：服务器访问密码、房间密码、字幕内容和协作数据都可能被途中读取或篡改。它只适合本机、隔离的开发环境或可信局域网，不应直接暴露到互联网。Aegisub 首次连接每个 `ws://` 地址时会要求用户确认风险。

服务端不会保存房间密码明文：客户端在建房时提交密码，数据库只保存 Argon2id 哈希。可选的服务器访问密码同样以 Argon2id 哈希配置。请勿把明文密码写入 Compose 文件、systemd unit、shell 历史或日志。

## 使用 Docker Compose 和 Caddy

把仓库中的 [`deploy`](../deploy) 目录复制到服务器，然后创建本地配置：

```sh
cd deploy
cp .env.example .env
cp server.env.example server.env
chmod 600 .env server.env
```

将 `.env` 中的 `COLLAB_DOMAIN` 改为 A/AAAA 记录已经指向该服务器的域名。生产环境应把 `SERVER_IMAGE` 固定到已发布的 `vX.Y.Z` 镜像。`latest` 可用于体验，`edge` 跟随默认分支，但两者都不是不可变的生产版本。

生成服务器访问密码哈希，不要把明文密码放入命令行参数：

```sh
printf '%s\n' '请替换为高强度密码' | \
  docker run --rm -i ghcr.io/wenhe233/aegisub-together-server:v0.0.5 hash-password
```

把输出写入 `server.env`，并保留单引号，防止哈希中的 `$` 被 shell 展开。`ARCHIVE_DAYS=0` 表示关闭冷归档，也是默认值；正整数表示房间无活动多少天后归档。归档会压缩房间状态并裁剪回放日志，但不会永久删除字幕，持有原房间凭据的客户端再次加入时会自动恢复。

检查配置并启动：

```sh
docker compose config --quiet
docker compose pull
docker compose up -d
curl --fail https://collab.example.com/healthz
```

Compose 网络为 Caddy 分配固定地址 `172.28.0.2`。服务端只信任该 `/32` 网段传来的转发地址，其他直连方不能伪造 `X-Forwarded-For` 绕过鉴权限流。如果修改网络布局，必须同时修改 Caddy 地址和可信代理 CIDR。不要把服务端容器的 8080 端口发布到公网。

### 备份与恢复 Compose 数据卷

管理命令通过 SQLite 在线备份生成一致副本，服务不需要停机。备份命令拒绝覆盖已有文件，因此每次应使用唯一文件名：

```sh
stamp=$(date -u +%Y%m%dT%H%M%SZ)
docker compose exec -T server /usr/local/bin/aegisub-together-server \
  backup -database /data/collab.db "/data/backup-$stamp.db"
docker compose cp "server:/data/backup-$stamp.db" "./backup-$stamp.db"
docker compose run --rm --no-deps server \
  rooms stats -database "/data/backup-$stamp.db"
```

请把备份复制到 Docker 主机之外，并定期验证。恢复时停止服务端，保留当前数据库及其 `-wal`、`-shm` 文件的副本，用已验证的备份替换命名卷中的 `collab.db`，将所有者设为 UID/GID `65532`，然后重新启动。不要直接复制运行中的 SQLite 数据库文件，在线备份必须使用 `backup` 命令。

## 使用原生二进制和 systemd

校验并安装 Release 中的二进制，然后创建专用系统账号：

```sh
sha256sum -c SHA256SUMS
sudo install -m 0755 aegisub-together-server-v0.0.5-linux-amd64 \
  /usr/local/bin/aegisub-together-server
sudo useradd --system --home /var/lib/aegisub-together \
  --shell /usr/sbin/nologin aegisub-collab
sudo install -d -m 0750 /etc/aegisub-together
sudo install -m 0644 deploy/systemd/aegisub-together-server.service \
  /etc/systemd/system/aegisub-together-server.service
sudo install -m 0600 deploy/systemd/server.env.example \
  /etc/aegisub-together/server.env
```

使用 `/usr/local/bin/aegisub-together-server hash-password` 生成哈希并编辑 `server.env`。安装 Caddy，将 `deploy/systemd/Caddyfile` 复制到 `/etc/caddy/Caddyfile`，并替换示例域名。默认 unit 仅监听 loopback，也只信任 loopback 代理。

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now aegisub-together-server caddy
sudo systemctl status aegisub-together-server caddy
curl --fail https://collab.example.com/healthz
```

unit 使用私有临时目录、只读操作系统文件系统、空 capability 集、系统调用和地址族限制，以及由 systemd 管理的状态目录。请在目标发行版上检查加固结果：

```sh
systemd-analyze security aegisub-together-server.service
```

原生部署的在线备份示例：

```sh
sudo install -d -o aegisub-collab -g aegisub-collab -m 0700 \
  /var/backups/aegisub-together
sudo -u aegisub-collab /usr/local/bin/aegisub-together-server backup \
  -database /var/lib/aegisub-together/collab.db \
  "/var/backups/aegisub-together/collab-$(date -u +%Y%m%dT%H%M%SZ).db"
sudo -u aegisub-collab /usr/local/bin/aegisub-together-server rooms stats \
  -database /var/backups/aegisub-together/VERIFIED-BACKUP.db
```

只能在服务停止时执行恢复。保留被替换的数据库和 WAL 文件，直到 `rooms stats`、服务启动、客户端入房和一次新备份全部成功。

## 在可信网络中直接使用 ws://

如果只在本机或可信局域网测试，可跳过 Caddy，让服务端直接监听受防火墙保护的地址：

```sh
aegisub-together-server serve \
  -listen 192.168.1.10:8080 \
  -database collab.db \
  -access-password-hash '<argon2id-hash>' \
  -archive-days 0
```

客户端填写 `ws://192.168.1.10:8080/v1/ws`。防火墙应只允许可信网段访问 8080；不要配置公网端口转发。只要流量会经过不可信 Wi-Fi、运营商网络、互联网或第三方代理，就应改用 Caddy 和 `wss://`。

## 运维与安全检查

- 持续更新 Caddy 和服务端镜像/二进制；升级前先在测试环境验证，并保留上一不可变镜像和数据库备份以便回滚。
- 安装 Release 文件前校验 `SHA256SUMS`，并把随版本发布的 SPDX JSON SBOM 与部署记录一同保存。
- 监控 Caddy 与服务/容器日志，但不要记录请求 payload、密码、房间快照或完整 WebSocket 帧。
- `wss://` 出现 TLS 证书警告代表部署失败。客户端不提供绕过选项，应修复 DNS、系统时间、证书链或 Caddy 配置。
- `/healthz` 有意只返回 `ok`。房间统计只能通过本机 `rooms stats` 命令获取。
- 邀请用户前，测试经 Caddy 的鉴权限流、数据库恢复、双客户端状态下的服务重启和 WebSocket Upgrade。
- 推送 `vX.Y.Z` 标签会在所有构建与测试成功后自动创建 GitHub Release；`v0.0.x` 标记为 Pre-release，`v0.1.0` 及以后按普通 Release 发布。
