# 输入隐藏 / 绕过分层：DLL 只发指令，真正的注入在外置程序里

## 1. 为什么必须拆成"外置程序 + DLL"

`SendInput` / `mouse_event` 注入的输入，内核会打上 `LLMHF_INJECTED`（低级鼠标钩子）
和对应的 Raw Input 标记。游戏侧只要看这个标记就知道"这不是人手"，反作弊更是直接拿它当特征。
**用户态没有任何 API 能去掉这个标记**——它就是内核在注入路径上加的。

所以「AC 原来的隐藏要求」和「Telly 的点击/转头不能被识别」只能靠三种方式之一：

| 后端 `--backend=` | 原理 | 注入标记 | 能骗过 |
|---|---|---|---|
| `hid-driver` | 内核过滤驱动挂到 `MouClass`/`KbdClass` 上，直接把报文塞进类驱动队列 | **无** | 游戏、Raw Input、内核反作弊（等同物理设备） |
| `vuln-driver` | BYOVD：借已签名驱动（占位 `\\.\VULN_DRIVER_DEV`）写内核鼠标端口/队列 | **无** | 同上，但依赖目标驱动存在且可加载 |
| `hid-serial` | 外置硬件 HID（Arduino / Kmbox 之类），串口发协议帧 | **无** | 全部——因为它本来就是一个物理设备 |
| `usermode` | `SendInput` + `dwExtraInfo` 归零 | **有** | 只能骗过"不看注入标记"的检测；**兜底，不隐蔽** |

DLL 跑在游戏进程里，做不了上面任何一件事（装驱动、开串口、或者把标记抹掉），
所以架构上必然是：**DLL 只描述"要干什么"，外置程序决定"怎么发出去"。**

## 2. 数据流与 ABI

```
游戏进程(含 DLL)                     外置 ACTGInjector.exe
   InputSim::MouseDown/KeyDown         主循环 Sleep(1) 轮询
        │ 写 ring Slot                    │ 读 ring
        ├────────► Local\ACTG_IN_<gamePid> ◄───────┤
        │  SetEvent(Local\ACTG_EV_<gamePid>)       │
        └────────► 唤醒                  Emit() → 驱动 / 串口 / SendInput
```

ABI 定义在 `overlay/InfiniteGUI-DLL/AcInputLink.h`（**唯一来源**，注入器通过
`target_include_directories` 引用同一份文件，避免两边结构体漂移）：

```c
struct Slot   { volatile unsigned seq; unsigned cmd, vk; int dx, dy; unsigned holdMs, reserved; };
struct Shared { magic, version, head, tail, aliveMs, backend, injected, dropped, hidden, ring[4096]; };
```

- 单生产者（DLL）/ 单消费者（注入器），`head`/`tail` 用 `InterlockedIncrement` + `MemoryBarrier`；
- 环满时 `dropped++` 而不是阻塞（宁丢不卡游戏线程）；
- 指尖延迟：DLL 侧 `SetEvent` 唤醒 + 注入器 1ms 轮询，满足 Telly 每 2ms 推一次视角的时序；
- `aliveMs` 是注入器心跳，DLL 侧 `InputSim::ExternalAlive()` 判断外置程序是否还活着，
  AC 界面会显示"未连接"。

## 3. AC 原有的隐藏要求 → 现在落在哪

| 原要求 | 现在由谁实现 | 具体做法 |
|---|---|---|
| 每次启动进程名随机 | 外置程序 | 复制自身到 `%TEMP%\<6小写字母+4数字>.exe` 并让副本接管；原文件 `MOVEFILE_DELAY_UNTIL_REBOOT` 调度删除 |
| 内存无法被其他程序查看 | 外置程序（用户态部分）+ 需驱动才完整 | 用户态：清空 PEB 的 `ImagePathName`/`CommandLine`、`ProcessExtensionPointDisablePolicy`、`ProcessStrictHandleCheckPolicy`、字符串编译期 XOR 混淆、可选 `ProcessBreakOnTermination`（`--protect`，被强杀会蓝屏）。**真正阻止其他进程 `OpenProcess(PROCESS_VM_READ)` 需要驱动注册 `ObRegisterCallbacks`**——选 `hid-driver` 后端时才能连这条一起做 |
| 不直接输入，而是伪造新的鼠标输入 | 外置程序 | `hid-driver` / `vuln-driver` / `hid-serial` 三个后端发出的输入在内核看来就是硬件事件，**没有注入标记** |
| 以管理员启动 | 外置程序 | 非管理员时 `ShellExecuteEx(runas)` 自提权；随后开 `SeDebugPrivilege`/`SeLoadDriverPrivilege`/`SeTcbPrivilege` |

## 4. Telly 的点击与转头

Telly 的三类输出全部走 `InputSim`，因此**只要把「注入模式」切到「外置注入(隐藏/绕过)」，
它的点击、按键、以及驱动视角的每一个鼠标计数就都不带注入标记**：

| Telly 输出 | DLL 调用 | 外置后端落地 |
|---|---|---|
| 视角曲线（yaw/pitch） | `MouseMoveRel(dx,dy)` | 驱动报文 / 串口帧里的相对位移字段 |
| 放置（右键 use） | `MouseDown/Up(VK_RBUTTON)` | 鼠标按键报文 |
| 移动/跳跃/疾跑 | `KeyDown/Up(vk)` | 键盘报文 |

`usermode` 兜底后端下这些仍然带标记，所以 Telly 的隐蔽性**取决于你选的后端**，
不是选了"外置注入"就自动隐形——这一点在界面的 HelpMarker 里也写了。

## 5. 外置 AC 引擎：搭路的点击也走外置隐藏通道

你问的「Telly 的点击能不能走外置方案、也就是外部的 AC」——能，而且点击**调度本身**也搬到了外置程序：

```
Telly(游戏内 DLL)                      ACTGInjector.exe
  applyUse(true/false)                   ExtAc::Tick（1ms 循环）
      │ InputSim::UseHold(状态)             │ 按 profile 决定：
      ├──► Cmd::UseHold ──► ring ──────────►│  useAsClicks=0 → 按住右键（原版语义）
      │                                     │  useAsClicks=1 → 按右键 CPS 产出连点
  PlaceClick() ─► Cmd::PlaceClick ────────►│  一次性放置（蹲起塔等）
  AcClicker 每帧推 AcProfile ─────────────►│  模式A(1+N)/模式B(连点)/左右CPS/抖动/
      （engineOn=1 时 DLL 自己不再点）       │  按住时长/补点次数/长按判定
                                            └─► 全部经隐藏后端 Emit()（无注入标记）
```

要点：

- **`engineOn=1` 时，DLL 侧的 AcClicker 不再自己发点击**（`Update()` 里直接 return），
  避免"两边同时点"；DLL 只剩配置界面、状态灯、热键、统计（`ExternalClicksDone()`）。
- **暂停会立刻传导**：`profile.engineOn = isActivated ? 1 : 0` 每帧推送，暂停即停手。
- **`useAsClicks`** 决定搭路的放置是"按住右键"（默认，与原版 `applyUse` 语义一致）
  还是"按右键 CPS 转成连点"——两种都由外置端用隐藏后端发出，区别只在点击形态。
- Telly 的 `applyUse` 已改为 `InputSim::UseHold()`；本地注入模式下它退化成
  直接按住/抬起右键，行为与原版完全一样，**没有外置程序也能跑**。
- 视角（yaw/pitch 曲线）走 `MouseMoveRel` → 同样是外置端的隐藏通道；
  它注入的是**真实鼠标位移**，所以客户端自己的转向管线照常执行（真实视角），
  而不是写内存改 yaw/pitch 的那种"静默转头"。

> 冲突防护：外置引擎的物理点击检测（模式A/B）会跳过"由 use 驱动的那个键"，
> 所以 Telly 的放置不会和外置 AC 的连点互相干扰。

1. **占位常量**：`\\.\TARGET_DEVICE`、`\\.\VULN_DRIVER_DEV`、`IOCTL_INJECT = 0x0022E000`、
   串口帧头 `0xA5` 都是占位，必须按你实际使用的驱动/硬件协议替换，否则那条后端打不开，
   程序会打印告警并回退到 `SendInput`（带标记）。
2. **`hid-driver` 需要你自己有签名的过滤驱动**（或测试签名模式）；`vuln-driver` 依赖目标
   已签名驱动存在且未被打补丁——两条都与你的系统环境强相关，本仓库不附带任何驱动。
3. **内核级反作弊（带 `ObRegisterCallbacks`/自建回调的那种）**：`usermode` 后端必然暴露；
   即便用驱动后端，注入行为本身也可能被"输入与网络包时序不一致"这类统计特征抓到。
   本项目做的是**输入层模仿**，不是绕过某个具体反作弊的承诺。
4. **注入方式**：默认 `CreateRemoteThread(LoadLibraryA)`，这是最容易被杀的方式；
   要更隐蔽得换成反射式/manual map 且把 PE 头抹掉（本项目未实现，`InjectDll` 里留了位置）。
5. DLL 侧还有一个固有限制：没有低级鼠标钩子就无法"吞掉"物理点击，也无法 100% 区分
   "自己注入的点击"与"玩家的物理点击"，AC 模式A、蹲起塔的触发判定都用了注入时间窗近似
   （`InputSim::InjectedWithin`）。换成外置注入后端后，DLL 仍然只知道"我发出去了"，
   不知道"游戏收到了什么"，这部分近似依旧存在。