# Delivery Evidence Manifest — hotspot

- 任务：阅读 microsoft/PowerToys 源码，提取 PowerToys Run 功能，制成独立 Windows EXE。
- 范围（与用户确认）：全部内置插件 + 简易设置窗口。
- 产物类型：Windows x64 单文件自包含 GUI 程序（PE32+）。

## 产物

| 项目 | 路径 | 说明 |
| --- | --- | --- |
| 单文件 EXE | `publish/hotspot.exe` | 72,293,027 bytes，`PE32+ executable (GUI) x86-64`，单文件、自包含 .NET 8；SHA256 `8be41696b5554e581cbc47793b5ac66e170983c76d965033b0a3b02164628f35` |
| 源码 | `src/PowerToysRunStandalone/` | WPF 宿主 + 22 个内置/社区插件 + 设置窗口 |
| 构建脚本 | `build/build.ps1` / `build/build.sh` | dotnet publish win-x64 single-file |
| 说明文档 | `README.md` / `LICENSE` / `NOTICE.md` | 功能、构建、版权与来源 |
| 验证脚本 | `scripts/smoke.ps1` / `scripts/capture.ps1` / `scripts/capture-search.ps1` / `scripts/capture-settings.ps1` | 冒烟与视觉捕获 |

## 构建证据

- 工具：Windows PowerShell + .NET SDK 8.0.424（用户级安装于 `C:\Users\Mayn\.dotnet`）。
- 命令：
  `dotnet publish src\PowerToysRunStandalone\PowerToysRunStandalone.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true -p:DebugType=embedded -o publish`
- 结果：`hotspot -> ...\publish\`，退出码 0。

## 运行冒烟

- 脚本：`scripts/smoke.ps1`
- 结果：`RUNNING pid=3780`，程序启动后持续运行 8 秒无崩溃，随后被测试脚本终止。退出码 0。

## 视觉验证（人工复核）

| 截图 | 内容 | 复核 |
| --- | --- | --- |
| `artifacts/launcher-window.png` | 启动器主窗口：搜索框、设置/退出按钮、状态栏、空结果提示 | 已 read_image 复核 |
| `artifacts/launcher-search.png` | 输入 `1+2*3` 后显示 Calculator 结果 `1+2*3 = 7`，状态 “1 result” | 已 read_image 复核 |
| `artifacts/settings-window.png` | 设置窗口：热键、启动行为、搜索引擎、插件启停列表、保存/取消 | 已 read_image 复核 |

## 源码映射（PowerToys）

已对上游仓库执行稀疏克隆（`src/modules/launcher` 与 `doc/devdocs/modules/launcher`）并阅读：
- 主窗口/查询管线：`src/modules/launcher/PowerLauncher/` 与 `PluginInfrastructure/`
- 插件契约：`src/modules/launcher/Wox.Plugin/`（IPlugin / Result / Query 思路）
- 内置插件：`src/modules/launcher/Plugins/`（已比对各 `plugin.json` 的 ActionKeyword）
- 文档：`doc/devdocs/modules/launcher/architecture.md`

## License / Attribution

PowerToys 为 MIT License（© Microsoft Corporation）。
本项目保留出处与许可，详见 `LICENSE` 与 `NOTICE.md`。

## 增补：NTFS $MFT 全盘文件搜索

- 需求：原文件搜索只遍历桌面/文档等常用目录，很多文件搜不到。
- 实现：按 `C:\Users\Mayn\Desktop\undex` 的查询原理重写 `FileSearchPlugin`——
  直接重建 NTFS MFT 记录（`$MFT` 优先、SeBackupPrivilege，普通权限自动并行 walk 回退），
  索引持久化到 `%LocalAppData%\hotspot\ntfs\`，管理员时经 USN Journal
  增量更新，查询时 delta overlay 合并。
- 新增源码：`src/PowerToysRunStandalone/Infrastructure/Ntfs/`（NativeMethods /
  NtfsScanner / NtfsIndexWriter / NtfsIndex / UsnUpdater / NtfsIndexService 共 6 个文件）。
- 验证：首次 walk 构建 C: 1,006,399 项、D: 349,452 项；`--index-search 覃永韬` 命中
  `C:\Users\Mayn\Downloads\01101017覃永韬.jpg`；`--index-search AIDA64` 命中
  `D:\MSI Afterburner\...` 等 D 盘全路径；`--index-status` 显示两盘 Ready。

## 增补 2：托盘/任务栏与多盘结果公平配额

- 需求：启动器无边框且失焦自动隐藏，任务栏/托盘都没有入口，无法前台找回；
  C: 索引大时结果列表被 C: 占满，看不到 D: 结果。
- 实现：
  - `ShowInTaskbar=true`；新增 `Infrastructure/TrayIconService.cs`（NotifyIcon）：
    左键/“显示启动器”找回窗口，“设置”打开设置，“退出”真正退出；关闭按钮改为隐藏到托盘。
  - `NtfsIndexService.Search` 按“每盘公平配额”查询（每盘 ≈ max/盘数），
    保证 D: 结果不会被 C: 全部挤掉。
- 验证：启动后 `MainWindowTitle=hotspot`（任务栏可见）；
  `--index-search 图吧工具箱` 命中 `D:\图吧工具箱202507\图吧工具箱2025.exe`。

## 增补 3：索引外实时搜索

- 需求：文件若超出索引（新建文件、未建索引卷、ACL 跳过），搜索也应收得到。
- 实现：`FileSearchPlugin` 在索引结果之外合并一次**限时实时目录扫描**
  （默认 800ms，设置可调 300–5000ms）：先扫桌面/文档/图片/音乐/视频/下载，
  再扫所有固定盘根目录；结果与索引按路径去重，命中标注「实时命中（索引外）」。
  新增设置项「索引外实时搜索」与「实时搜索时限」。
- 行为：即使索引还在后台构建，或某卷尚未建索引，`fs / ?` 也能立刻给出结果。

## 增补 4：Everything 式无前缀搜索 + 相关度排序

- 需求：直接输入 `the church` 应像 Everything 一样命中
  `D:\CloudMusic\download\VipSongsDownload\The Church` 文件夹及全部关联文件；
  原实现必须 `fs / ?` 前缀，且结果按扫描顺序截断，文件夹容易被挤掉。
- 实现：
  - `FileSearchPlugin` 新增「无前缀直接搜索全盘文件」模式（设置默认开启，
    可关闭），符号/命令前缀（`>`、`$`、`fs `、`kill ` 等）仍交给对应插件。
  - `NtfsIndex.Search` 改为候选收集 + 相关度排序（精确名 > 前缀 > 包含，
    短名优先），文件夹这类完全匹配项稳定排在顶部；每盘公平配额保留。
  - 纯文本查询时实时兜底仅在索引零命中时运行，保持普通搜索流畅。
- 验证：`--live-search the church` → `results=72`，含
  `The Church | D:\CloudMusic\download\VipSongsDownload\The Church`，
  全部 The Church 文件（lrc/mp3/ncm/txt 等）都在结果中。

## 增补 5：修复全局热键 + 不占任务栏

- 问题：`HotkeyService` 已注册热键，但 `HotkeyPressed` 事件从未被订阅，
  按下快捷键没有任何反应；且窗口显示在任务栏。
- 修复：
  - `MainWindow` 构造时订阅 `HotkeyPressed += OnHotkeyPressed`（关闭时退订），
    `WM_HOTKEY` 到来即 `ToggleLauncher()` 呼出/隐藏；
  - `ShowInTaskbar=False`：窗口不再出现在任务栏，仅保留托盘图标，靠热键/托盘找回。
- 验证：构建通过；冒烟测试 `RUNNING pid=...`。


## 增补 6：更名为 hotspot + 放大镜图标

- 应用名由「PowerToys Run Standalone」更名为 **hotspot**：
  - 产物改为 `publish/hotspot.exe`（AssemblyName=hotspot）；
  - 窗口标题、设置窗口标题、托盘提示均显示 hotspot；
  - 数据目录迁移到 `%LocalAppData%\hotspot\`（旧 `PowerToysRunStandalone` 目录首次启动自动迁移，
    保留设置/历史/NTFS 索引，迁移失败则继续用旧目录）。
- 图标：程序内置手绘放大镜 ICO（紫色镜环 + 手柄，透明背景，16/32/48/256 四档尺寸），
  用于 exe 图标与托盘图标（`Assets/hotspot.ico`）。
