# Aegisub Together 协议 v2

协议 v2 继续承载于 `/v1/ws` WebSocket 端点。它与 v1 不兼容，客户端与服务端必须同时升级。所有 envelope 包含 `protocol_version: 2`、`type`、`request_id`、`room_revision` 和 `payload`；未知的必需消息类型必须拒绝，payload 中未知字段可以忽略。

文本 JSON、超过 32 KiB 时使用的 `0x01 + zlib(JSON)` 二进制帧、64 MiB 解压上限、鉴权、房间、字幕批次、维护、批注与审计语义沿用 v1。

## 原子锁集合

- `lock_set_request` 携带完整的 `line_ids`、可空的 `active_line_id` 和递增 `generation`。`line_ids` 必须唯一且不超过 10,000 个。
- 锁启用时，服务端以该集合完整替换请求者现有锁。任一行已由其他成员持有时，释放请求者旧锁、拒绝整个新集合，并返回所有已发现冲突；不允许部分授予。
- `lock_set_state` 是某成员完整锁状态。`granted: true` 时 `line_ids` 是当前集合且 `conflicts` 为空；拒绝时 `line_ids` 为空并返回冲突持有者。
- `room_joined` 包含当前 `lock_sets` 和 `presence`，加入者不需要等待后续广播才能绘制正确状态。
- 锁关闭时不建立硬锁，`active_line_id` 只更新 presence。
- 锁启用时 `modify`、`delete`、`move` 必须由目标行锁持有者提交。断开、正常退出、维护模式和 60 秒无交互过期都会释放完整集合。

`schema.json` 是线协议 Schema，`errors.json` 是稳定错误注册表，`fixtures/` 和 `fixtures-invalid/` 是跨语言黄金样例。
