# Aegisub Together 开发、测试与发布规范

本文档是协作功能后续开发的执行基准，供维护者和自动化开发代理使用。若本文与 GitHub Actions 工作流不一致，应先核对工作流并在同一提交中修正文档。

## 支持范围

- 协作客户端只支持、构建和测试 Windows。不要为 Linux 或 macOS 客户端增加兼容性门禁。
- 协作服务端使用 Go，发布 `linux/amd64`、`linux/arm64` 二进制和同架构 GHCR 镜像。
- Windows 客户端使用 WinHTTP，允许 `wss://` 和 `ws://`；公网必须优先使用 WSS。
- Go 工具链固定为 1.26.5；正式打包使用 Meson 1.11.2。第三方依赖必须锁定版本、来源和哈希。

## 提交规范

- 在 `feat/collaboration` 分支开发，不重写已发布标签或远端历史。
- 使用 Conventional Commits，例如 `feat(client): ...`、`fix(server): ...`、`test(protocol): ...`、`docs(release): ...`。
- 每个提交必须是可测试的完整功能或修复，并包含相应测试；禁止 WIP、临时调试和“随后再补测试”的提交。
- 提交前检查 `git diff --check` 和 `git status --short`，不得混入构建产物、密码、字幕内容、凭据或无关修改。
- 协议不兼容变更必须同时更新协议文档、JSON Schema、黄金夹具、Go 类型、C++ 类型及部署升级说明。

## 本地验证

### 协议

```powershell
python -m pip install jsonschema
python protocol/v2/validate_fixtures.py
```

### 服务端

```powershell
Push-Location server
go mod verify
go test -count=1 ./...
go test -race -count=1 ./...
Pop-Location
```

SQLite 恢复、归档和备份用例不得跳过。涉及存储或房间生命周期时，还要单独运行工作流中列出的恢复测试集合。

### Windows 客户端

首次配置应使用空构建目录，确认 fallback 依赖仍可下载并通过哈希验证：

```powershell
meson setup build -Dbuildtype=release -Dcollaboration=enabled -Ddefault_library=static --force-fallback-for=zlib,harfbuzz,freetype2,fribidi,libpng
meson compile -C build
meson test -C build --verbose "gtest main"
```

协作改动至少运行 `collaboration_*.*` 测试。网络层改动还必须运行仅测试使用的 WinHTTP smoke：启动临时 Go 服务端和 SQLite，完成访问认证、建房、入房、消息收发、心跳与正常退出。

正式候选包必须执行 `win-installer` 和 `win-portable`，并检查 zip 至少包含：

- `aegisub.exe`
- `locale/zh_CN/LC_MESSAGES/aegisub.mo`

## GitHub Actions 门禁

- `Meson CI`：Windows MSVC Release 和 wxWidgets master 两套完整编译与测试；stable 任务额外运行 WinHTTP smoke。
- `Collaboration server CI`：协议夹具、Go 测试、race、SQLite 恢复、多架构镜像构建；默认分支或标签才推送镜像。
- `Collaboration release packages`：协议和服务端验证、Windows portable、Linux 两架构服务端、SHA256SUMS、SPDX SBOM；只有标签构建发布 GitHub Release。
- CI 冷缓存构建通常需要 45–60 分钟。任务仍在正常下载、编译或输出日志时不得取消；长任务约每十分钟查看一次。
- 瞬时网络失败可重跑；代码、测试、依赖哈希或打包错误必须修复并形成独立 Conventional Commit，不能仅靠重复运行掩盖。

## 发布流程

1. 更新 `docs/releases/vX.Y.Z.md` 和部署示例；不兼容协议升级必须写明客户端、服务端需要同时升级。
2. 推送 `feat/collaboration`，等待所有必需 CI 成功；需要时手动运行一次 `Collaboration release packages` 预跑。非标签预跑只生成 artifact，不创建 Release。
3. 在已经通过验证的提交创建 annotated tag：`git tag -a vX.Y.Z -m "Aegisub Together vX.Y.Z"`。
4. 推送新标签。不得移动、覆盖或强推任何已存在标签。
5. 等待标签触发的 Release 和 GHCR 工作流完成。`v0.0.x` 必须是非草稿 Pre-release；从 `v0.1.0` 起为普通 Release。
6. 检查 GitHub Release 包含 Windows portable、Linux amd64/arm64 服务端、`SHA256SUMS` 和 SPDX JSON SBOM；检查 GHCR 存在版本、minor 和 `latest` 多架构标签。

发布工作流只有 `publish-release` job 可获得 `contents: write`；其他 job 保持只读。任何密码只能位于 GitHub Secrets、环境变量或 Windows Credential Manager，不得进入配置、日志、artifact 或发布说明。
