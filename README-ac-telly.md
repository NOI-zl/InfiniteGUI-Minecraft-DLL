# AC 自动点击 + 搭路(Telly) + 蹲起塔 —— 已整合进本仓库

三个模块**直接整合进 `InfiniteGUI-DLL/` 工程**（新建模块文件 + 注册 + 工程条目），
隐藏/绕过部分是仓库里的**独立程序 `ACTGInjector/`**（自己运行，负责提权、隐藏、注入、发输入）。

## 1. 新增进工程的模块

| 文件 | 作用 |
|---|---|
| `InfiniteGUI-DLL/AcInputLink.h` | DLL ↔ 外置程序 共享内存 ABI（唯一来源） |
| `InfiniteGUI-DLL/InputSim.h/.cpp` | 统一输入注入层（左右键区分正确 + 外置隐藏后端 + 外置 AC 引擎接口） |
| `InfiniteGUI-DLL/AcClicker.h/.cpp` | **AC 自动点击**：模式A(1+N) / 模式B(连点)、左右键独立 CPS、抖动、状态灯(红绿灯) |
| `InfiniteGUI-DLL/Telly.h/.cpp` | **搭路**：21 帧曲线、setup 12 tick、激活手势、原版提示文案 |
| `InfiniteGUI-DLL/SneakTower.h/.cpp` | **蹲起塔**：跳跃 → 潜行脉冲 → 右键放置 → 间隔 |
| `InfiniteGUI-DLL/ItemManager.cpp` | 已插入三个模块的注册（`AddItem(&X::Instance())`） |
| `InfiniteGUI-DLL/InfiniteGUI-DLL.vcxproj` | 已插入 5 个 ClCompile + 5 个 ClInclude 条目 |

模块遵循本工程既有写法（`WindowModule, UpdateModule, KeybindModule, AffixModule, Item, SoundModule` + `static X& Instance()`），
所以它们会像 Sprint 那样直接出现在菜单里。

## 2. 外置程序（隐藏/绕过就在这里）

```
ACTGInjector/ACTGInjector.exe --backend=hid-driver --dll=InfiniteGUI-DLL.dll
```

它做的事（DLL 在游戏进程里做不到的部分）：

- 每次启动**随机进程名**（自复制到 `%TEMP%` 后用副本接管）
- 自动**提权**（`runas` + `SeDebugPrivilege`/`SeLoadDriverPrivilege`）
- 用户态隐藏：清空 PEB 镜像路径/命令行、进程缓解策略、可选进程保护（`--protect`）
- 注入 `InfiniteGUI-DLL.dll` 到 Minecraft，并建立命令环
- **外置 AC 引擎**：点击调度（CPS/抖动/按住时长/左右键）跑在这里，经
  `hid-driver`(内核过滤驱动) / `vuln-driver`(BYOVD) / `hid-serial`(外置硬件) 发出，
  **没有 `LLMHF_INJECTED` 注入标记**（`usermode` 兜底后端仍带标记）

`--backend` 的驱动设备名 / IOCTL 码 / 串口帧在 `main.cpp` 里是**占位常量**，需按你实际用的驱动或硬件替换。

## 3. 用法

1. 开游戏 → 运行 `ACTGInjector.exe`（会自己提权、找游戏、注入、开命令环）
2. 游戏内打开菜单：
   - **自动点击(AC)**：激活键默认 `F8`（切换启用/暂停，状态灯绿=启用 红=暂停）、切换模式键 `F9`
   - **搭路(Telly)**：按住潜行蹲稳 → 提示变绿 → 再按住右键触发；停止键默认 `X`
   - **蹲起塔**：热键默认 `Z`
3. 三个模块的「注入模式」都有第 4 项 **外置注入(隐藏/绕过)**：选中后点击、转头、放置
   全部由外置程序用隐藏后端发出。AC 界面会显示当前后端、连接状态、外置点击计数。

## 4. 编译

- **Actions**：任何分支 push 都会跑 `.github/workflows/build.yml` → 出 DLL + `ACTGInjector.exe` + 打包。
  里面的「校验整合是否到位」步骤会检查模块文件、注册、工程条目、UTF-8 BOM，缺任何一样直接报错。
- **本地**：`msbuild InfiniteGUI-DLL.sln /m /p:Configuration=Release /p:Platform=x64`
  注入器：`cmake -S ACTGInjector -B build-injector -A x64 && cmake --build build-injector --config Release`

## 5. 从上游重新同步代码后（可选）

如果哪天把上游 `InfiniteGUI-Minecraft-DLL` 的新提交拉下来、覆盖掉了这两个被打过补丁的文件，
用仓库里的脚本把整合重新叠回去（幂等）：

```powershell
# 上游副本目录 <upstream>，本仓库的模块文件作为来源
tools\apply_overlay.ps1 -RepoRoot <upstream> -OverlayDir .\InfiniteGUI-DLL
```

## 6. 诚实边界

| 项 | 说明 |
|---|---|
| 搭建过程 | 新模块文件是本地写的，g++ 语法检查通过；`AcClicker/Telly/SneakTower` 依赖 ImGui/nlohmann，**真编译在 Actions 上**（本机无 Windows SDK） |
| 模式A 不拦截物理点击 | DLL 无低级鼠标钩子，改成物理点击后补 N 次；切外置注入后最干净 |
| Telly 读世界数据 | 本 DLL 设计上不读游戏内存 → 「俯角≥75°+站在边缘」由玩家保证；`Auto Swap`/`Show Activation Hitbox`/`Disable SafeWalk` 保留原名原默认值但不生效 |
| 隐藏强度 | 取决于所选后端；`usermode` 仍带注入标记，驱动/硬件后端才无标记 |
| 外置引擎关闭时 | DLL 侧仍能自己点（本地/转发模式），不影响单用 DLL 的玩法 |

详细原理见 `docs/ac-telly-port.md`（eps-plus Telly 剖析与移植映射）与 `docs/ac-telly-bypass.md`（输入隐藏分层）。