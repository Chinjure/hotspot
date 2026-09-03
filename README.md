# PowerToys Run Standalone

从 [microsoft/PowerToys](https://github.com/microsoft/PowerToys)（MIT License）的
PowerToys Run（Launcher）功能中提取、独立化的快速启动器。

目标是：**一个单独的 Windows EXE**，双击即用，不依赖 PowerToys 主程序。
默认全局热键：`Alt+Space`。

> 本项目独立封装了 PowerToys Run 的交互模型与插件体系：
> 主窗口（`PowerLauncher`）→ 插件基础设施（`Wox.Plugin`/`IPlugin`）→ 内置插件。
> 由于原版设置界面寄生在 PowerToys Settings 应用中，本项目额外实现了一个轻量设置窗口。

## 功能

- 全局热键呼出/隐藏（默认 `Alt+Space`，可在设置中修改）
- 边框透明、TopMost、居中显示的启动器窗口
- **任务栏按钮 + 托盘图标**：窗口显示时有任务栏按钮；隐藏后托盘图标常驻，
  左键/“显示启动器”找回，“设置”打开设置，“退出”真正退出
- 输入即搜索、`↑↓` 选择、`Enter` 执行、`Esc` 隐藏
- **Everything 式文件搜索**：直接输入 `the church` 这类纯文本也会搜全盘文件
  （无需 `fs` / `?` 前缀；设置里可关闭）；结果按“精确 > 前缀 > 包含”排序，
  文件夹与所有匹配文件都能出现在列表里
- 结果可复制到剪贴板（计算器、时间、环境变量等）
- 近期使用历史（History 插件）
- 简易设置窗口：热键、开机自启、搜索引擎、插件启停
- 配置：`%LocalAppData%\PowerToysRunStandalone\settings.json`
- 历史：`%LocalAppData%\PowerToysRunStandalone\history.json`

## 内置插件

| 插件 | 用法示例 | 对应 PowerToys Run 插件 |
| --- | --- | --- |
| Program | `notepad` | Microsoft.Plugin.Program |
| Web Search | `?? PowerToys Run` | Community.PowerToys.Run.Plugin.WebSearch |
| Calculator | `1+2*3` 或 `= sqrt(16)` | Microsoft.Plugin.Calculator |
| Unit Converter | `u 100 km to mi` / `100kg to lb` | Community.PowerToys.Run.Plugin.UnitConverter |
| Window Walker | `< notepad` / `ww notepad` | Microsoft.Plugin.WindowWalker |
| File Search / NTFS $MFT | `? report.docx` / `fs report.docx` | Microsoft.Plugin.Indexer（重写为 NTFS MFT 全盘索引） |
| Folder | `folder desktop` | Microsoft.Plugin.Folder |
| Registry | `: Notepad` / `reg Notepad` | Microsoft.Plugin.Registry |
| Process Killer | `kill notepad` | Microsoft.Plugin.ProcessKiller（历史内置） |
| Shell | `> dir C:\` | Microsoft.Plugin.Shell |
| Windows Settings | `$ display` / `win display` | Microsoft.Plugin.WindowsSettings |
| Time & Date | `) time` / `time` / `date` | Microsoft.Plugin.TimeDate |
| Environment Variable | `env PATH` | Microsoft.Plugin.EnvironmentVariable（历史内置） |
| URI | `// https://github.com` / `uri https://github.com` | Microsoft.Plugin.Uri |
| System | `system` / `uptime` | Microsoft.Plugin.System |
| History | `!!` / `history` | Microsoft.Plugin.History |
| ValueGenerator | `# guid` / `# md5 hello` / `# b64 hello` | Community.PowerToys.Run.Plugin.ValueGenerator |
| Windows Terminal | `_` / `_ dir` | Microsoft.Plugin.WindowsTerminal |
| OneNote | `o:` / `o: notes` | Microsoft.Plugin.OneNote |
| PowerToys | `@` / `@ color` | Microsoft.Plugin.PowerToys |
| Service | `!` / `! start <name>` | Microsoft.Plugin.Service |
| VSCode Workspaces | `vsc <text>` | Community.PowerToys.Run.Plugin.VSCodeWorkspaces |

插件可以在设置窗口里逐个开关。

## 构建（win-x64 单文件 EXE）

依赖：.NET 8 SDK（Windows/Linux 均可交叉编译 WPF，
项目已启用 `EnableWindowsTargeting=true`）。
未安装 SDK 时，可在 Windows PowerShell 中运行
`powershell -ExecutionPolicy Bypass -File scripts\install-dotnet.ps1`
（安装到用户目录，无需管理员权限）。

```bash
# 方式 1：PowerShell（Windows）
./build/build.ps1

# 方式 2：bash（Linux/WSL）
./build/build.sh
```

产物：

```
publish/PowerToysRunStandalone.exe   # 单文件、自包含 win-x64
```

也可以手动执行：

```bash
dotnet publish src/PowerToysRunStandalone/PowerToysRunStandalone.csproj \
  -c Release -r win-x64 --self-contained true \
  -p:PublishSingleFile=true \
  -p:IncludeNativeLibrariesForSelfExtract=true \
  -p:EnableCompressionInSingleFile=true \
  -o publish
```

## 文件搜索：NTFS $MFT 全盘索引

`? <name>` 与 `fs <name>` 不再只遍历桌面/文档等常用目录，而是直接重建
NTFS 记录索引（与 [undex](../undex/) 的查询原理一致）：

1. **全量扫描**：优先以 SeBackupPrivilege 打开 `\\?\C:\$MFT`，流式解析每条
   FILE 记录（`0x30 FILE_NAME` 取文件名/父目录引用/大小/时间，`0x80 DATA`
   判定普通文件），通过父引用链重建完整路径——覆盖卷上全部文件与目录。
2. **walk 回退**：Win11 24H2+ 管理员保护或普通权限打不开 `$MFT` 时，自动
   降级为并行目录递归（记录每条目的 NTFS FileId），无管理员也能完整建索引。
3. **持久索引**：索引存于 `%LocalAppData%\PowerToysRunStandalone\ntfs\`
   （`C-meta.json` / `C-index.dat` / `C-delta.json`），首次启动后台构建，
   之后启动直接内存映射加载，搜索为全盘毫秒级。
4. **USN 增量**：以管理员运行时读取 `FSCTL_READ_USN_JOURNAL` 追加增量
   （新增/删除/改名/更新），查询时按 overlay 合并；无权限时自动跳过。
5. **索引外实时搜索**：每次 `fs / ?` 查询还会做一次限时（默认 800ms，可在设置
   调整）的实时目录扫描，合并索引未覆盖的文件——新建文件、未建索引的卷、因
   ACL 跳过的路径都能立刻找到；结果标注「实时命中（索引外）」。不需要时可在
   设置里关闭。
6. **范围**：默认索引所有固定 NTFS 卷（C:、D: …），设置窗口可查看每个卷的
   状态并一键"立即重建索引"；也可取消勾选「启用全盘文件搜索」退回常用目录遍历。
   多盘结果按“每盘公平配额”混排，大容量的 C: 不会挤掉 D: 的结果。
7. **Everything 式查询**：默认开启「无前缀直接搜索全盘文件」，直接输入文件名
   即可命中（`the church` → `D:\CloudMusic\download\VipSongsDownload\The Church`
   文件夹及其全部 70 个文件）；结果按“精确名 > 前缀 > 包含”排序，
   文件夹排在完全匹配的顶部。

## 源码结构

```
PowerToysRunStandalone/
├── src/PowerToysRunStandalone/
│   ├── App.xaml(.cs)              # 启动/装配（对应 PowerLauncher App）
│   ├── Infrastructure/
│   │   ├── PluginTypes.cs         # IPlugin/IResult/Query/PluginManager
│   │   ├── SettingsService.cs     # JSON 配置（替代 PowerToys Settings）
│   │   ├── HotkeyService.cs       # RegisterHotKey 全局热键
│   │   ├── HistoryService.cs      # 近期结果历史
│   │   └── Ntfs/                  # NTFS $MFT 全盘索引
│   │       ├── NativeMethods.cs   # Win32 P/Invoke（$MFT/USN/特权/遍历）
│   │       ├── NtfsScanner.cs     # $MFT 记录解析 + walk 回退
│   │       ├── NtfsIndexWriter.cs # 紧凑二进制索引写入
│   │       ├── NtfsIndex.cs       # 内存映射搜索/全路径重建/delta overlay
│   │       ├── UsnUpdater.cs      # USN Journal 增量更新
│   │       └── NtfsIndexService.cs# 后台建索引/加载/搜索编排
│   ├── Plugins/                   # 内置插件（每个插件一个文件）
│   └── UI/
│       ├── MainWindow.xaml(.cs)   # 启动器主窗口
│       └── SettingsWindow.xaml(.cs)
├── build/build.ps1
├── build/build.sh
├── LICENSE
└── NOTICE.md
```

## 与 PowerToys 源码的映射

- 主窗口/查询管线：`src/modules/launcher/PowerLauncher/` 与
  `src/modules/launcher/PluginInfrastructure/`
- 插件接口：`src/modules/launcher/Wox.Plugin/`（`IPlugin`、`Result`、`Query`）
- 内置插件：`src/modules/launcher/Plugins/`
- 模块文档：`doc/devdocs/modules/launcher/architecture.md`

## 验证

```powershell
# 冒烟测试：启动 exe 并确认 8 秒内不崩溃
.\scripts\smoke.ps1

# 视觉捕获（可选参数 --stay 防止失焦自动隐藏）
.\scripts\capture.ps1        # 空窗口截图
.\scripts\capture-search.ps1 # 输入计算表达式后截图
```

## 系统要求

- Windows 10 1809+ / Windows 11（x64）
- 无 PowerToys 依赖；首次运行自动创建配置
- 单文件 EXE 已包含 .NET 运行时（自包含）

## License

PowerToys 使用 MIT License。本项目基于 PowerToys 的源码与架构做独立封装，
保留原始版权声明，详见 [LICENSE](LICENSE) 与 [NOTICE.md](NOTICE.md)。
