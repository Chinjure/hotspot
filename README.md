# hotspot (C++/Win32 原生版)

这是 hotspot 启动器的 **C++ 原生重写版**：仅保留文件搜索 + NTFS $MFT 全盘索引 +
主窗口 + 全局热键 + 托盘 + 设置 + 历史 + CLI 模式，界面使用 Win32 + Direct2D/DirectWrite。
构建只依赖 **Windows SDK + MSVC（Visual Studio C++ 工具集）**，通过 `build.ps1` 调用
`vswhere → vcvarsall → cl/link/rc`，零第三方依赖。

## 构建

在 Windows PowerShell 中：

```powershell
.\build.ps1
```

产物：

```
publish\hotspot-cpp.exe   # 原生 PE32+ GUI x64
```

要求：已安装 Visual Studio 的“使用 C++ 的桌面开发”工作负载（MSVC x64 + Windows SDK）。

## 功能

- 文件搜索：`fs <name>`、`? <name>`、无前缀 Everything 式搜索（设置可关）
- NTFS $MFT 全盘索引：优先 `$MFT` + SeBackupPrivilege，失败自动 walk 回退
- 索引持久化与 C# 版**二进制兼容**：直接复用
  `%LocalAppData%\hotspot\ntfs\C-index.dat / C-meta.json / C-delta.json`
- USN Journal 增量更新（管理员权限时），Delta overlay 合并
- 索引外实时搜索（默认 800ms 时限，300–5000ms 可调）
- 多盘公平配额 + 相关度排序（精确名 > 前缀 > 包含，短名优先）
- 无边框、圆角、TopMost 启动器窗口（Direct2D 自绘结果列表）
- 全局热键（默认 `Alt+Space`，可修改）
- 托盘图标：左键显示启动器，右键 显示/设置/退出
- 设置窗口：热键、开机自启、启动时显示、搜索引擎（兼容字段）、
  NTFS 索引开关/实时搜索/无前缀搜索/时限/重建、File Search 与 History 插件开关
- 历史：执行结果写入 `%LocalAppData%\hotspot\history.json`；`history` / `hist` / `!!` 显示最近 30 条
- 数据目录与旧版自动迁移到 `%LocalAppData%\hotspot`

## CLI 模式

日志写入 `%LocalAppData%\hotspot\ntfs\`（与原版同名日志文件）：

```
hotspot-cpp.exe --index-build [C]
hotspot-cpp.exe --index-search <name>
hotspot-cpp.exe --index-status
hotspot-cpp.exe --live-search <name>
hotspot-cpp.exe --settings       # GUI：启动后打开设置
hotspot-cpp.exe --stay           # GUI：保持窗口（截图/调试用，不失焦隐藏）
```

## 源码结构

```
hotspot/
├── build.ps1
├── README.md
└── src/
    ├── main.cpp / app.{h,cpp}        # WinMain、GUI/CLI 分派
    ├── json.{h,cpp}                  # 最小 JSON（UTF-8，兼容 .NET 序列化）
    ├── settings.{h,cpp}              # settings.json + HKCU Run 自动启动
    ├── history.{h,cpp}               # history.json
    ├── hotkey.{h,cpp}                # RegisterHotKey
    ├── tray.{h,cpp}                  # Shell_NotifyIcon
    ├── winutil.{h,cpp}               # 路径/编码/字符串/文件工具
    ├── file_search.{h,cpp}           # fs/?/无前缀 + 实时兜底 + 历史
    ├── ui/
    │   ├── main_window.{h,cpp}       # Direct2D 启动器窗口
    │   ├── settings_window.{h,cpp}   # Win32 设置窗口
    │   ├── resource.h / app.rc / app.manifest
    └── ntfs/
        ├── ntfs_types.h
        ├── ntfs_win32.{h,cpp}        # Win32 原生封装
        ├── ntfs_scanner.{h,cpp}      # $MFT / walk 扫描
        ├── ntfs_index_writer.{h,cpp} # 兼容二进制索引写入
        ├── ntfs_index.{h,cpp}        # 内存映射搜索/路径重建/Delta
        ├── usn_updater.{h,cpp}       # USN Journal 增量
        └── ntfs_service.{h,cpp}      # 后台编排/状态/CLI
```

## 与 C# 版的关系

- C++ 版不会删除 C# 版源码；两者并存，`cpp/` 为独立原生工程。
- 数据文件格式保持一致：settings.json、history.json、`X-meta.json`、
  `X-delta.json`，以及 `X-index.dat`（`PRSIDX01` 头 + 40B 记录 + UTF-16LE 名字区）。
- 本版暂不包含 C# 版的其余 21 个插件（计算器/Web 搜索/窗口切换等）。
