# Aegisub Together 协议 v1

协议 v1 是承载于 `/v1/ws` WebSocket 端点的 JSON 消息协议。服务端使用单调递增的 `room_revision` 对房间内所有持久事件排序。每个被接受的字幕批次只占用一个 revision，并且原子应用。

## 帧格式

- 控制消息和不超过 32 KiB 的 payload 使用 UTF-8 JSON 文本帧。
- 较大的批次与快照 envelope 使用二进制帧。第一个字节为 `0x01`，其余字节是包含 UTF-8 JSON envelope 的 zlib 流。
- 解压后的单个 envelope 不得超过 64 MiB。
- 每个 envelope 都包含 `protocol_version`、`type`、`request_id`、`room_revision` 和 `payload`。
- 客户端忽略未知的 payload 字段，但必须拒绝未知的消息 `type`。

WebSocket 可以通过 `wss://` 或 `ws://` 连接，帧格式和应用协议完全相同。`wss://` 提供 TLS；`ws://` 不加密，只应在可信网络中使用。

## 连接顺序

1. 第一条应用消息必须是 `access_auth`，并在五秒内到达。服务器未配置访问认证时，密码使用空字符串。
2. 服务端返回 `access_ok` 后，客户端发送 `create_room` 或 `join_room`。
3. 建房或入房成功后返回 `room_joined`，其中包含权威快照和当前房间 revision。
4. 持久事件广播为 `batch_applied`、`comment_changed` 或 `reindex`。临时的锁、presence、心跳和维护消息不会递增房间 revision，除非对应消息另有明确规定。

## 顺序与冲突规则

- Dialogue 的身份是 `line_id`，数组索引永远不是身份。规范格式为 10 字符 Crockford Base32 的 50-bit 客户端前缀、连字符和一个正的 64-bit 无符号十进制计数，例如 `9K3MT7Q2CD-128`。小写、含歧义的 Base32 字符、前导零和越界计数均会被拒绝。
- `insert` 和 `move` 携带客户端已知的 `left_id` 与 `right_id`。服务端分配规范的 16 字符 Base62 `pos_key`，并在 `batch_applied` 中返回。
- `modify`、`delete` 和 `move` 携带客户端观察到的行版本。
- `batch_applied.operations` 中的每一项都包含规范化后的输入 `operation`；当该行仍然存活时，还包含完整的规范 `line`。整节替换会在同一项中返回新的节版本。
- 如果分配位置时触发全房间 reindex，`batch_applied` 会包含 `positions`，即批次应用后最终文档中完整的“行 ID 到位置键”映射。接收方必须原子应用 operations 和该映射。
- Styles 和 Script Info 是具有独立版本的整节值。
- 任一校验失败都会通过 `batch_rejected` 拒绝整个批次。
- 发起方只能根据 `batch_applied` 推进 confirmed shadow，发送 `submit_batch` 时不得推进。

## 维护模式仲裁

- `maintenance_request` 将全房间独占写租约授予第一个请求者，并释放所有行锁。其他成员保持连接，但其批次和锁请求会以 `maintenance_active` 拒绝。
- 持有者发送 `maintenance_release` 正常结束。非持有者可以先发送 `maintenance_cancel_request`，经过服务端声明的 30 秒宽限期后再发送 `maintenance_cancel_force`。
- 只有持有者成功持久化的批次会续期 10 分钟空闲租约。心跳不会续期，60 分钟硬上限也不会移动。
- 持有者断线、空闲过期、硬上限到期、主动释放或强制取消时，服务端都会广播非活动的 `maintenance_state`，且不递增房间 revision。

## 批注与离线对账

- `comment_create` 把留言和可选的字幕文本建议附加到创建者观察到的精确行版本。即使另一成员持有行锁也允许创建批注。对应行被删除后，批注仍保留在快照中。
- 采纳建议需要持有当前行锁，并且 base 行版本未变化。文本修改、行版本递增和批注状态变化共享同一个房间 revision 和 SQLite 事务。拒绝或解决 open 批注不要求行锁。
- `snapshot_request` 始终返回完整权威 `snapshot_state`，用于三方离线对账。`audit_request` 按单调递增 ID 分页返回脱敏后的房间审计事件。两类请求都不会修改房间 revision。
- 五分钟内重连时，客户端可以提交房间签发的 resume token，以恢复相同的 member ID。离线期间批注只读，不进入对账队列。

`schema.json` 是线协议的规范 schema，`errors.json` 是稳定错误码注册表，`fixtures/` 中的文件是跨语言规范示例。
