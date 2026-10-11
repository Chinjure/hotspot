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
- 路径搜索：查询里出现 `\`（`/` 等价）即按路径搜索，例如
  `C:\Users\me\Desktop\proj`、`src\components\button.tsx`；末尾带 `\`
  表示列出该文件夹及其内容；绝对路径按字面前缀限定范围，相对路径允许
  省略中间目录。结果排序**先看路径匹配度**（精确名/精确茎 > 前缀 > 包含 >
  子项），同一匹配档内才是 **exe/lnk > 文件夹 > 其它文件**
- NTFS $MFT 全盘索引：优先 `$MFT` + SeBackupPrivilege，失败自动 walk 回退
- 索引持久化与 C# 版**二进制兼容**：直接复用
  `%LocalAppData%\hotspot\ntfs\C-index.dat / C-meta.json / C-delta.json`
- USN Journal 增量更新（管理员权限时），Delta overlay 合并
- 索引外实时搜索（默认 800ms 时限，300–5000ms 可调）
- 多盘公平配额 + 相关度排序：**名字匹配优先于文件类型**（精确名/精确茎 >
  前缀 > 包含；同档内才比 exe/lnk > 文件 > 文件夹；短名优先），所以搜 `clock`
  时完全名为 `clock` 的文件夹排在 `Clock Widget.lnk` 之前
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
hotspot-cpp.exe --index-search <name|path>   # 含 \ 时按路径搜索
hotspot-cpp.exe --index-status
hotspot-cpp.exe --live-search <name|path>    # 含 \ 时按路径搜索
hotspot-cpp.exe --settings       # GUI：启动后打开设置
hotspot-cpp.exe --stay           # GUI：保持窗口（截图/调试用，不失焦隐藏）
```

## 验证

```powershell
.\verify\path-query-test.ps1     # 路径匹配与排序单元测试（毫秒级，不需要索引）
.\verify\path-search-check.ps1   # 端到端：真实 exe + 临时夹具 + 真实索引，检查顺序与范围
.\verify\focus-keys.ps1          # GUI：焦点/键盘回归（点空白、右键菜单后 Esc 与输入仍有效）
.\verify\capture.ps1 -Mode search -Query "C:\Users\me\Desktop" `
                     -Out artifacts\cpp-path-search.png
```

## 源码结构

```
hotspot/
├── build.ps1
├── README.md
├── verify/                          # 验证脚本（单元测试 / 端到端 / 截图）
│   ├── path_query_test.cpp          # 路径匹配 + 排序单元测试
│   ├── path-query-test.ps1          # 编译并运行上面的单元测试
│   ├── path-search-check.ps1        # 端到端路径搜索检查
│   └── capture.ps1                  # GUI 截图
└── src/
    ├── main.cpp / app.{h,cpp}        # WinMain、GUI/CLI 分派
    ├── json.{h,cpp}                  # 最小 JSON（UTF-8，兼容 .NET 序列化）
    ├── settings.{h,cpp}              # settings.json + HKCU Run 自动启动
    ├── history.{h,cpp}               # history.json
    ├── hotkey.{h,cpp}                # RegisterHotKey
    ├── tray.{h,cpp}                  # Shell_NotifyIcon
    ├── winutil.{h,cpp}               # 路径/编码/字符串/大小写折叠工具
    ├── path_query.{h,cpp}            # \ 触发的路径查询解析与匹配分级
    ├── file_rank.{h,cpp}             # 结果排序（名字模式 / 路径模式）
    ├── file_search.{h,cpp}           # fs/?/无前缀 + 路径搜索 + 实时兜底 + 历史
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
