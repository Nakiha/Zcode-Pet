# 数据源

UI 只消费 `Session` / `Snapshot`。`data_sources.cpp` 负责选择、探查、合并和配置；`zcode_source.cpp` 保留原 ZCode 读取逻辑；`deepseek_source.cpp` 独立处理 Harness 会话。读某一个来源失败，不中断另一个来源。

右键宠物 → 数据源：分别勾选 ZCode / DeepSeek Harness。默认允许自动探查两种来源，未发现的来源休眠；手动取消勾选会保存到 `%LOCALAPPDATA%/ZCodePet/settings.ini` 的 `[Sources]`，启动探查不覆盖选择。每 30 秒只探测两个已知位置，不扫描安装盘、注册表或全进程列表。

- ZCode：`%USERPROFILE%/.zcode/v2/tasks-index.sqlite` 及原有日志/只读状态兜底。
- DeepSeek Harness：从 `%USERPROFILE%/.dsh/storages/session_projcache/sessions` 读取小型投影记录，可通过桌宠进程继承的 `DSH_HOME` 指定根目录，支持 `~` 展开。不会读取账号、令牌或启动 Harness。

Harness 的官方来源依据为 [deepseek-ai/deepseek-harness](https://github.com/deepseek-ai/deepseek-harness/tree/f3803c96974ad4a94195fb97e5c637e9c2a3afe3)：`packages/bundle/base/cordis.patch.yml`、`packages/util/home-paths`、`packages/session/session-persistence-jsonl`、`packages/core/session` 和 `packages/interaction/user-approval`。

目前读取当前 v4 的 `sessions/<project>/<session>/session.v4.jsonl[.zstd]`。支持默认 Zstandard 压缩，以及未压缩配置；只提交完整帧/完整 JSON 行，半写入尾部等下轮补全。识别会话名称、turn 开始/完成/错误/中断，以及持久化的 approval 询问/决定。审批等待与普通问答不是同一种事件；没有持久化证据的插件询问不会猜成等待态。子代理不加入顶层会话列表。

Harness 行显示 `DS ·` 标识；内部来源字段和独立 workspace 命名空间避免与 ZCode 混淆。完成事件复用现有提醒和宠物动作。为了不把进程重启前未结束的旧 turn 当作当前工作，运行态需要匹配当前 Harness 窗口进程的启动时间；没有识别到桌面进程时保留历史会话并标为不确定。不会仅凭有一个未结束 turn 就宣称进程仍在运行。

限制：已从官方源码构建 Desktop，并在独立开发目录通过官方 headless profile 访问本地 Mock Chat API；实测同一会话新 turn 的运行→完成、完成气泡和宠物完成动作。Desktop 已进入工作区并显示 Mock Chat 模型；桌面输入框发送与生产安装包尚未验收。该读取方式依赖持久化检查点，官方未刷盘的活动可能延迟出现；不代表实时运行内存状态。当前适配当前格式 v4，不负责历史格式迁移、自定义插件状态投影、归档数据库或多 profile 合并。Harness 点击标题/提醒正文通过 UIA 定位“会话 / Sessions”树中的 TreeItem（不要求键盘可聚焦），优先调用 SelectionItemPattern::Select；不支持该模式时回退到 Invoke、默认动作或只向确切条目发送 Enter。名称匹配兼容“进行中 + 标题 + 时间”等已知状态装饰，并拒绝可能对应其他已知会话的名称。只接受已验证的 Harness 窗口；同名会话拒绝猜选，折叠、未挂载或离屏条目返回提示。ZCode 仍使用原有项目分组/ListItem 路径。

性能边界：无需安装插件或额外配置。启动只枚举一次投影缓存目录，之后使用 ReadDirectoryChangesW 返回的具体文件名读取变化的小缓存；不递归遍历 sessions，不定时重扫缓存目录。只有 Windows 通知缓冲溢出时重新对账，缓存静默时不重复读取正文。最多保留最近 80 个缓存记录和游标，展示最多 40 个会话。

标题优先使用投影缓存中的 title（ver=1），校验 formatVersion、createdAt、cwd、isSeeded、session id 和 seq；旧缓存不能覆盖日志中已确认的较新标题。即使标题事件在 1.17 MiB 大压缩帧中，显示标题也不需要先找到该事件。缓存可能按 Harness 的策略延迟约 5 秒；缓存版本不兼容时不能保证取到标题。

状态仍保留定点日志核对：从缓存的 cwd/id 按官方编码规则算出精确文件路径，不遍历会话目录找文件。变化日志用于确认实时 turn/结束原因，静默日志只检查已知文件元数据，不重新读取正文。现有持久化缓存不能单独保证实时状态，官方桌面连接地址/认证由内部 IPC 提供，也不是无需授权的固定第三方端口，因此没有冒用缓存来承诺实时推送。大日志定点读取仍采用头部与尾部恢复、有限预算和缓冲；证据不足时保留同步/未知标记。

顺序读取缓存未解码的数据，只按新增磁盘读取扣每轮 4 MiB I/O 预算。解码约 8 ms 分片并轮转服务文件，固定目标水位代替不断移动的 EOF；保留上次确认状态并以“同步中”标注追赶，中间历史状态不触发新提醒。有积压时以 200 ms 节拍追赶，无积压恢复 2 秒；ZCode 查询另行缓存，最多每 2 秒一次。时间预算在工作单元之间检查，单帧解析或磁盘 I/O 可超过预算，不是硬实时保证。无法保证追上任意写入速率，但不再重复读取缓存，也不要求重放全部历史后才能使用尾部状态。

验证：`--data-sources-test` 覆盖状态、审批、改名、新 turn、Zstd 压缩/非压缩 block、增量半帧、截断、格式拒绝以及静默重复检查；`--source-benchmark` 另在当前目录写无会话内容的 `source-benchmark.txt`。Zstandard 静态解码器来源与完整许可位于 `third_party/zstd`，许可也内嵌到 EXE。

## 本地 mock 复现

`python scripts/mock-chat-api.py --port 18766` 只监听 loopback，提供 OpenAI Chat Completions 和 Messages 测试应答；不需要真实 API Key，日志不记录请求正文。`tests/harness-mock.patch.yml` 为隔离的开发/测试 profile 添加 Local Mock 模型，并选为 headless 默认模型。不要把此 overlay 应用于真实账号或生产配置。

在已编译的 Harness 源码目录，用独立 `DSH_HOME` 启动：`pnpm dsh headless --patch <本项目>/tests/harness-mock.patch.yml --json "Local mock test"`，再用 `--session-id` 恢复返回的会话 ID。桌宠也继承相同 `DSH_HOME`。源码 Desktop 的进程为 Electron，需额外将 `ZCODEPET_HARNESS_EXECUTABLE` 设为该实例的绝对 EXE 路径；只接受该明确文件的身份，不泛化匹配所有 Electron 应用。正式安装版仍验证 DeepSeek Harness.exe。

`ZCodePet.exe --source-diagnostic` 在当前目录写 15 个每秒的来源计数样本（会话数、运行、完成、未知、错误标记），不写会话名称、正文或密钥。2026-10-29 本机恢复测试中，第 4–13 秒运行数为 1，第 14 秒完成数为 1；运行实例探针显示 completedNotices=1、spineTransitions=5、spineErrors=0。审批与坏帧等分支由单元测试覆盖，本轮未用真实模型或真实审批操作制造事件。

## 大日志与导航回归

`--data-sources-test` 增加约 65 MiB 历史、追加事件、持续增长、零 I/O 预算时消费已有缓存的回归。`--source-benchmark` 实测历史文件 68,239,672 字节，近期状态初始化读取 2,162,688 字节；下一条事件追加读取 62 字节，静默检查不读取正文。数字为合成日志读取器测试，不代表所有真实日志、CPU 或磁盘性能。

`--harness-navigation-smoke` 为手动测试入口：会打开真实 Harness 侧栏唯一的“新会话”条目，不能放进无人值守 CTest。本机源码 Desktop 上返回 Opened、唯一匹配 1 个，实际动作是 SelectionItemPattern::Select；模型测试另覆盖 Harness 同名拒绝和不依赖目录名的定位准备。未自动展开或滚动列表，也未验证其他语言的会话树名称。


无需插件路径回归：新增缓存版本/身份/seq 校验、目录通知驱动的改名更新，以及连续空闲 Poll 不新增缓存正文读取的测试。本机源码 Harness 开发数据通过缓存索引发现 3 个会话，15 秒诊断稳定为 2 个完成、0 个未知、0 个来源警告。单 EXE 保留，未安装或修改 Harness 插件。
