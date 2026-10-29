# 数据源

UI 只消费 `Session` / `Snapshot`。`data_sources.cpp` 负责选择、探查、合并和配置；`zcode_source.cpp` 保留原 ZCode 读取逻辑；`deepseek_source.cpp` 独立处理 Harness 会话。读某一个来源失败，不中断另一个来源。

右键宠物 → 数据源：分别勾选 ZCode / DeepSeek Harness。默认允许自动探查两种来源，未发现的来源休眠；手动取消勾选会保存到 `%LOCALAPPDATA%/ZCodePet/settings.ini` 的 `[Sources]`，启动探查不覆盖选择。每 30 秒只探测两个已知位置，不扫描安装盘、注册表或全进程列表。

- ZCode：`%USERPROFILE%/.zcode/v2/tasks-index.sqlite` 及原有日志/只读状态兜底。
- DeepSeek Harness：`%USERPROFILE%/.dsh/sessions`，可通过桌宠进程继承的 `DSH_HOME` 指定根目录，支持 `~` 展开。不会读取账号、令牌或启动 Harness。

Harness 的官方来源依据为 [deepseek-ai/deepseek-harness](https://github.com/deepseek-ai/deepseek-harness/tree/f3803c96974ad4a94195fb97e5c637e9c2a3afe3)：`packages/bundle/base/cordis.patch.yml`、`packages/util/home-paths`、`packages/session/session-persistence-jsonl`、`packages/core/session` 和 `packages/interaction/user-approval`。

目前读取当前 v4 的 `sessions/<project>/<session>/session.v4.jsonl[.zstd]`。支持默认 Zstandard 压缩，以及未压缩配置；只提交完整帧/完整 JSON 行，半写入尾部等下轮补全。识别会话名称、turn 开始/完成/错误/中断，以及持久化的 approval 询问/决定。审批等待与普通问答不是同一种事件；没有持久化证据的插件询问不会猜成等待态。子代理不加入顶层会话列表。

Harness 行显示 `DS ·` 标识；内部来源字段和独立 workspace 命名空间避免与 ZCode 混淆。完成事件复用现有提醒和宠物动作。为了不把进程重启前未结束的旧 turn 当作当前工作，运行态需要匹配当前 Harness 窗口进程的启动时间；没有识别到桌面进程时保留历史会话并标为不确定。不会仅凭有一个未结束 turn 就宣称进程仍在运行。

限制：已从官方源码构建 Desktop，并在独立开发目录通过官方 headless profile 访问本地 Mock Chat API；实测同一会话新 turn 的运行→完成、完成气泡和宠物完成动作。Desktop 已进入工作区并显示 Mock Chat 模型；桌面输入框发送与生产安装包尚未验收。该读取方式依赖持久化检查点，官方未刷盘的活动可能延迟出现；不代表实时运行内存状态。当前适配当前格式 v4，不负责历史格式迁移、自定义插件状态投影、归档数据库或多 profile 合并。Harness 的点击跳转尚未接入，点击会明确提示，不会误操作 ZCode；原 ZCode UIA 导航保留。

性能边界：通过目录名称变更通知发现新会话，不因正文追加重扫；30 秒扫描作通知不可用时的兜底。发现文件每轮最多 256 个目录项并以约 4 ms 分片；保留最近 80 个日志游标、展示最多 40 个会话。读取阶段约 8 ms 分片、轮转服务各文件，一轮最多读取 4 MiB 压缩输入，单帧解压上限 4 MiB，每个日志的帧解码约 2 ms 分片。时间预算在工作单元之间检查，单次磁盘 I/O / 大帧解析仍可能超过预算，不是硬实时保证。未变化文件只查元数据，不打开正文或再次解析；日志替换/截断会重新定位。解析失败缓存到文件再次变化，避免反复处理同一损坏文件。

验证：`--data-sources-test` 覆盖状态、审批、改名、新 turn、Zstd 压缩/非压缩 block、增量半帧、截断、格式拒绝以及静默重复检查；`--source-benchmark` 另在当前目录写无会话内容的 `source-benchmark.txt`。Zstandard 静态解码器来源与完整许可位于 `third_party/zstd`，许可也内嵌到 EXE。

## 本地 mock 复现

`python scripts/mock-chat-api.py --port 18766` 只监听 loopback，提供 OpenAI Chat Completions 和 Messages 测试应答；不需要真实 API Key，日志不记录请求正文。`tests/harness-mock.patch.yml` 为隔离的开发/测试 profile 添加 Local Mock 模型，并选为 headless 默认模型。不要把此 overlay 应用于真实账号或生产配置。

在已编译的 Harness 源码目录，用独立 `DSH_HOME` 启动：`pnpm dsh headless --patch <本项目>/tests/harness-mock.patch.yml --json "Local mock test"`，再用 `--session-id` 恢复返回的会话 ID。桌宠也继承相同 `DSH_HOME`。源码 Desktop 的进程为 Electron，需额外将 `ZCODEPET_HARNESS_EXECUTABLE` 设为该实例的绝对 EXE 路径；只接受该明确文件的身份，不泛化匹配所有 Electron 应用。正式安装版仍验证 DeepSeek Harness.exe。

`ZCodePet.exe --source-diagnostic` 在当前目录写 15 个每秒的来源计数样本（会话数、运行、完成、未知、错误标记），不写会话名称、正文或密钥。2026-10-29 本机恢复测试中，第 4–13 秒运行数为 1，第 14 秒完成数为 1；运行实例探针显示 completedNotices=1、spineTransitions=5、spineErrors=0。审批与坏帧等分支由单元测试覆盖，本轮未用真实模型或真实审批操作制造事件。
