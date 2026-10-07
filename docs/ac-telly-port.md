# eps-plus 的 Telly 是怎么做的，以及本移植怎么落地

来源：`https://github.com/liuyyx/eps-plus`（26.2.x 分支）
`common/src/main/java/.../modules/impl/movement/Telly.java`（3727 行，174 KB）

## 1. 它是什么

源码注释说明它是从 `RavenBS-Plus-Plus` 的 1.8.9 脚本 `BSLegitTellyFix` 移植过来的
**「脚本 Telly」**：不是 Scaffold 那种算法搭路，而是**把"人手的整套操作"按帧演出来**——
所以它必须驱动**真实视角**（`player.setYRot/setXRot`），不能走静默转头，
否则客户端表现和脚本对不上（脚本的每个动作都建立在视角已经被转动的前提上）。

模块注册为 `Telly`，类别 `MOVEMENT`，单例 `Telly.INSTANCE`。

## 2. 三条曲线（21 帧，逐值搬过来了，一个数都没改）

| 曲线 | 21 个值 |
|---|---|
| `YAW_CURVE` | 91.68, 98.88, 78.94, 37.45, 1.61, −21.69, −33.98, −35.80, −34.64, −33.85, −33.06, −31.55, −29.26, −26.65, −24.19, −21.07, −18.84, −17.06, −8.87, 2.61, 41.94 |
| `PITCH_CURVE` | 64.31, 59.95, 60.57, 61.46, 60.64, 58.89, 56.91, 56.63, 58.65, 61.63, 64.20, 66.74, 68.69, 70.64, 73.01, 75.37, 77.46, 78.56, 78.90, 77.22, 72.25 |
| `FORWARD_CURVE` | 1,1,0,0,−1,−1,−1,−1,−1,−1,−1,−1,−1,−1,−1,−1,−1,−1,−1,−1,1 |
| `STRAFE_CURVE` | −1,−1,−1,−1,0,0,0,0,0,0,0,0,0,0,0,0,0,−1,−1,−1,−1 |

每 tick（50ms，20 TPS）的判定（`onPostPlayerInput`）：

```java
strafe    = STRAFE_CURVE[phase];
sprinting = (phase == 0 || phase == 1);
jumping   = (phase >= 1 && phase <= 19);
use       = (phase >= 7);
applyMovement(FORWARD_CURVE[phase], strafe, jumping, sprinting);
applyUse(use);
nextPhase = (phase + 1) % 21;
setRotationTarget(baseYaw + YAW_CURVE[nextPhase], PITCH_CURVE[nextPhase], 50L);
```

另有两条护栏：`isPlayerOffLane() || isPlayerAheadOfBridge()` → `applyMovement(0,0,false,false)`
（容差 1，故意保留）。

## 3. 触发前的 setup 阶段（12 tick）

`SETUP_INERTIA_TICKS = 6`：

- tick 0–11：`applyMovement(-1.0, -1.0, jump = (tick >= 6), false)` + `applyUse(true)`
- 每 tick 旋转目标 `(baseYaw, 74.52)`，50ms；tick 11 时改成 `(baseYaw + YAW_CURVE[19], PITCH_CURVE[19])`
- 结束后：`setupTick = -1`、`takeoverDetectionAt = now + 125`、`cyclePhase = 19`

## 4. 激活手势（和它的提示文案）

`activationPitch() = 75.0`。触发条件（`beginAutomation`）：

1. 手持物是方块（否则 `&cHold blocks before starting`）
2. `player.getXRot() >= 75`（看下方）
3. 看向边缘 + `isShiftKeyDown()`（蹲住）
4. 按住时间到 `Activation Time`，且 yaw 已吸附到 `Snap Degrees` 网格
5. 再按住右键 → `disableSafeWalkForRun()` → `beginAutomation()`

> 文案注释里专门写了一行：**必须按住潜行蹲稳 → 提示变绿后再按住右键**，
> 不是"松开潜行"（脚本原版是松开，本移植改掉了），别再写成 release。

触发瞬间把 yaw 对齐到网格并**绝对**写入：`baseYaw = round((yaw − 46.5)/90)*90 + 46.5`，
注释说明不能把触发瞬间的瞄准偏差固化进 `baseYaw`，否则桥会越搭越歪。
默认吸附值是 **46.5 而不是 45**：45 是整度数，吸附后动作太"机器"，
实测被 Intave 报 acting computer-like——所以这个值是有原因的，不要"顺手改成 45"。

## 5. 提示与通知（原文照抄）

- 屏幕提示（`drawActivatePrompt`）：
  - 未就绪 → `"Hold " + keyDisplay("sneak")`，红 `0xFF5555`
  - 就绪 → `keyDisplay("sneak") + " + " + rightMouseDisplay()`，绿 `0x55FF55`
  - 位置 `x = guiScaledWidth/2 - width/2`、`y = guiScaledHeight/2 + 10`，scale 1.0，
    200ms 淡入淡出、alpha 下限 16
- 诊断行：Debug 开时画在 `guiScaledHeight - 32`，白色居中
- 通知（`printStatus`，标题一律 `"Telly"`，`&a`成功 `&c`错误 `&e`警告）：

| 时机 | 原文 |
|---|---|
| 武装完成 | `Armed. Hold <sneak> at the edge looking down; when green hold <右键>` |
| 没拿方块 | `Hold blocks before starting` |
| 开始搭 | `Started` |
| 停止 | `Stopped. Hold <sneak> looking down to arm again` |
| 自动放 | `AutoPlace activated` |

停止时还会打一行 `STOP reason=… phase=… setupTick=… tick=… placedOk=… placeFail=… failHist=… pos=… vH=… fall=… holdBlock=…`；
其他常量：`SENSITIVITY_QUANTUM = 0.03404715`、`YAW_NUDGE_PATTERN = {0,1,-1,2,-2}`、
`ACTIVATION_ACROSS_MIN = 0.38`、自适应瞄准 125ms、ghost-block 取消、`Render3DScheduler` 画命中框。

## 6. 移植到 DLL 的映射表

| Java 侧 | DLL 侧（本仓库） | 说明 |
|---|---|---|
| `player.setYRot/setXRot` | `InputSim::MouseMoveRel(dx,dy)` | 角度→鼠标计数：`(sens*0.6+0.2)^3*8*0.15` 度/计数 |
| `baseYaw`（绝对吸附值） | `baseYaw = 0`（触发瞬间为相对零点） | 曲线只用到相对偏移，行为一致；绝对网格吸附不可得 |
| `player.getXRot()` 初值 | 触发时**假定** `pitch = 75.0`（= `activationPitch()`） | 有了这个基准，`PITCH_CURVE` 就能当绝对 pitch 用 |
| `applyMovement(forward, strafe, jump, sprint)` | `setHeld(gameVk(...), bool)` | 走 `GameKeyBind` 的按键映射；玩家自己也按着同一键时不强抬 |
| `applyUse(bool)` | `applyUse(bool)` → 右键 down/up | 每 tick = 50ms 定时器 |
| 50ms 游戏 tick | `kTickMs = 50` 的固定步进 | 20 TPS 假设，允许校时 |
| `printStatus` → `NotificationManager` | `NotificationItem::AddNotification` | 提示弹窗按类型自动取标题，故正文前加 `Telly: ` |
| `keyDisplay("sneak")` | `vkName(sneakVk())` | Shift/Ctrl/Space…；`sneakVk` 优先读游戏设置 |
| `SendPositionEvent`（发包） | —（不做） | DLL 不发游戏包 |
| 世界/背包读取、SafeWalk、ghost-block、lane 护栏 | —（不做，界面标注） | 本 DLL 架构上不读游戏内存 |

## 7. 为什么不做「读内存直接改玩家状态」

上游 `InfiniteGUI-Minecraft-DLL` 的设计前提就是**不碰游戏内存**：它只替换 WndProc、
装 Raw Input、在 `wglSwapBuffers` 上叠 ImGui，游戏状态靠窗口/输入行为推断。
读 `LocalPlayer`/`ItemStack`/`World` 需要偏移扫描或 Java 侧注入，两样都与这个前提冲突，
也会把 DLL 从"输入层脚本"变成"内存外挂"，暴露面完全不同。
所以上面的映射表里，凡是依赖世界数据的项都**保留设置名与默认值、界面写明当前不生效**，
而不是悄悄删掉。