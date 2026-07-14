# Aegisub 在线协作版实施计划

## 1. 基线与已修正的设计问题

- 正式编码前从 `8165f1a` 新建 `feat/collaboration`，保留现有 `feature` 分支，不重写远端历史。
- 先完善并提交 `docs/aegisub-collab-spec.md`；其后每个可测试功能完成即单独提交，全部使用 Conventional Commits。
- v1 同步核心 ASS：Dialogue、Styles、Script Info 和协作 Extradata；不传输附件、视频或 Aegisub Project Garbage。
- shadow 只能在服务器确认后推进，客户端使用 `confirmed shadow + pending batches + projected shadow`。
- 协作模式不用 Aegisub 原生全文件快照 undo，改用本成员已确认批次的逆 op。
- 行锁 60 秒无交互释放，presence 继续保留；批次拒绝后回滚并保存恢复 `.ass`。
- Styles 和 Script Info 使用独立节版本；位置键由服务器分配并支持 reindex 屏障。
- 建房以当前文件初始化；加入已有房前处理未保存内容，快照只加载到内存。

## 2. 协议、状态与安全接口

- WebSocket 入口为 `/v1/ws`。消息统一包含 `protocol_version`、`type`、`request_id`、`room_revision` 和 payload。
- 控制消息使用文本 JSON；超过 32 KiB 的批次和快照使用 zlib 压缩二进制 JSON，解压上限 64 MiB。
- 每个成功原子批次递增一次房间 revision；同批 op 共享 revision。Dialogue op 为：
  - `modify(id, fields, base_version)`
  - `insert(id, left_id, right_id, data)`
  - `delete(id, base_version)`
  - `move(id, left_id, right_id, base_version)`
  - `restore(id)`
  - `replace_styles(styles, base_version)`
  - `replace_script_info(entries, base_version)`
- 行 ID 使用持久化的 50-bit Base32 客户端前缀加自增计数，存入 `_aegi_collab_id`；顺序键存入 `_aegi_collab_pos`。
- 客户端按顺序维护待确认队列；任一批次被拒后，该批及其依赖批次写入恢复副本，重新请求快照。
- 鉴权第一条消息始终为服务器访问认证；失败或 5 秒超时直接关闭。房间不存在与密码错误返回相同错误。
- 房名 NFC 规范化、大小写敏感、1–64 字符；昵称 1–32 字符且房内唯一；房间密码 8–128 字节。
- 密码使用 Argon2id（16-byte salt、64 MiB、3 iterations、parallelism 2）。每 IP 五分钟内 10 次失败后封禁 15 分钟。
- SQLite 使用 WAL；每房间由单 goroutine 串行定序，批次在单事务中持久化。每 500 批或 5 分钟生成快照。
- Windows 客户端使用 WinHTTP WebSocket/TLS；网络线程只收发，字幕与 UI 只在主线程修改。
- 密码默认只驻留内存；“记住密码”使用 Windows Credential Manager。

## 3. 功能实施与提交边界

每个条目连同自动化测试完成后立即提交，不产生 WIP 提交：

1. `docs(spec): finalize collaboration architecture`
   - 写入同步边界、协议、状态机、超时、恢复和发布决策。
2. `docs(plan): add collaboration implementation plan`
   - 将本执行计划纳入仓库，作为提交和验收清单。
3. `feat(protocol): define collaboration protocol v1`
   - 增加协议文档、JSON Schema、错误码、黄金消息样例和跨语言测试夹具。
4. `feat(server): add authenticated room lifecycle`
   - 实现访问认证、建房/入房、当前文件初始化、唯一昵称、快照和统一鉴权错误。
5. `feat(server): sequence and persist atomic subtitle batches`
   - 实现核心 ASS、版本校验、事务批次、墓碑、restore、ID 重铸和快照恢复。
6. `feat(server): add canonical line ordering`
   - 实现 Base62 顺序键、并发插入、anchor 降级和 reindex 屏障。
7. `feat(server): enforce locks and presence`
   - 实现焦点锁、60 秒空闲租约、30 秒心跳回收和锁关闭模式。
8. `feat(server): add maintenance arbitration`
   - 实现全房冻结、10 分钟空闲、60 分钟上限和取消请求/强制取消。
9. `feat(server): add comments and offline reconciliation support`
   - 实现留言、建议文本、状态流和重连对账接口。
10. `feat(server): add archival and administration commands`
    - 增加 `serve`、`hash-password`、`backup`、`rooms stats/archive/unarchive` 和冷归档。
11. `feat(client): add stable collaboration metadata`
    - 实现 ID/位置 extradata、清洗、快照、diff 和 op 应用。
12. `feat(client): add windows collaboration transport`
    - 实现 WinHTTP WSS、协议编码、线程队列、重连、Credential Manager 和 Meson 选项。
13. `feat(client): add room connection workflow`
    - 增加建房、入房、昵称和密码 UI，以及安全的本地文档切换。
14. `feat(client): synchronize confirmed and pending document state`
    - 接入 commit/活动行事件，实现双 shadow、ack/reject、ID 映射和远端主线程应用。
15. `feat(client): enforce collaborative editing locks`
    - 实现编辑器、时间控件、网格命令的只读和 presence 展示。
16. `feat(client): add maintenance and rejection recovery`
    - 增加维护 UI、mutation guard、冻结横幅和恢复副本。
17. `feat(client): add collaborative undo and redo`
    - 协作模式使用本成员已确认批次的逆 op；离线单机保留原生 undo。
18. `feat(client): reconcile offline edits`
    - 持久化离线基线，执行三方比较并在短暂维护模式下原子提交结果。
19. `feat(client): add line comments and suggestions`
    - 增加批注数量、留言、建议文本、采纳、拒绝和解决 UI。
20. `ci(server): publish multi-architecture server images`
    - PR/push 测试镜像；版本标签推送 GHCR amd64/arm64 镜像。
21. `ci(release): package collaboration releases`
    - 同一标签生成 Windows zip、Linux 服务端二进制、校验和、SBOM 和 GitHub Release。
22. `docs(deploy): document compose and systemd deployments`
    - 提供非 root Dockerfile、Caddy Compose、systemd unit、环境文件和备份流程。

## 4. 测试与验收

- 服务端协议测试覆盖鉴权、限流、房名竞态、批次原子性、版本、锁、维护模式、ID 重铸、restore、并发插入、reindex、SQLite 回放、冷归档和资源限制。
- C++ 单元测试覆盖 ID 清洗、核心 ASS diff、双 shadow、拒绝恢复、逆 op undo、顺序、节版本和离线三方比较。
- Go 与 C++ 必须通过同一套黄金消息夹具；未知字段可忽略，未知必需消息类型安全拒绝。
- 双客户端实测覆盖粘贴、Duplicate、切分、删除后撤销、TPP、模板机、批次拒绝、维护冻结、焦点锁、逐字输入、样式同步、挂机释放、恢复副本、服务重启、离线冲突、批注和锁关闭冲突。
- 安全验收包含 TLS 校验、密码不落普通配置或日志、统一鉴权错误、Caddy 后真实 IP 限流、日志脱敏和数据库备份恢复。
- 性能门槛：一万行快照、同位置连续插入一万次、单行 commit 不等待 ack、三客户端持续编辑 30 分钟无丢批或 shadow 漂移。
- M1–M3 全部完成并通过真实双人工作流后统一发布 `v0.1.0`，不提前发布 M2 alpha。

## 5. 固定假设与范围外事项

- 行锁策略建房后不可变，默认开启；房内任何成员可申请维护模式。
- 房间持久存在；可配置冷归档但不自动永久删除。
- 不做账号系统、Web 管理后台、CRDT/OT、字段级实时合并、视频/附件同步、行内共同编辑或额外 `lua_api` 合并。
- 批注不做讨论串和整行字段补丁；采纳建议必须通过普通行锁与版本校验。
- 协作功能首版只承诺 Windows；其他平台必须继续编译并保持离线原版行为。
- 服务端固定 Go 1.26.5，第三方模块必须锁定在 `go.mod`/`go.sum`，工具链不得提交进仓库。
