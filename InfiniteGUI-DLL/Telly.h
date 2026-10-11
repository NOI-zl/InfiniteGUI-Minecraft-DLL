#pragma once

#include <Windows.h>

#include "imgui\imgui.h"
#include "Item.h"
#include "WindowModule.h"
#include "AffixModule.h"
#include "UpdateModule.h"
#include "KeybindModule.h"
#include "SoundModule.h"
#include <string>
#include <set>
#include <chrono>
#include "KeyState.h"
#include "InputSim.h"

// ============================================================================
// Telly（搭路）—— 移植自 eps-plus 26.2.x 的 movement/Telly.java
//
// 原实现是**脚本 Telly**：靠驱动玩家视角 + 注入键鼠，按固定的 21 帧曲线把
// 「后退搭桥」这一套人手操作演出来（源码注释原文：模拟人手，理论可绕任何反作弊）。
// 本模块把同一套曲线、同一套激活手势、同一套提示文案搬到 DLL 侧。
//
// 保真部分（逐值对齐，未改动）：
//   * 21 帧 YAW / PITCH / FORWARD / STRAFE 四条曲线
//   * setup 阶段 12 tick（后退+左移，第 6 tick 起跳，右键全程按住）
//   * 每 tick 判定：sprinting = phase 0/1、jumping = phase 1..19、use = phase >= 7
//   * 激活手势时序：按住潜行计时 → Activation Time → 提示由红转绿 → 再按住右键触发
//   * 屏幕提示原文：`Hold <sneak>` / `<sneak> + <右键>`（红 0xFF5555 / 绿 0x55FF55，
//     屏幕中心下方 +10px，200ms 淡入淡出，alpha 下限 16）
//   * 通知原文：Armed. / Hold blocks before starting / Started / Stopped. / AutoPlace activated
//   * 设置项与默认值：Auto Swap / Disable SafeWalk / Show Activation Hitbox / Print /
//     Debug Log / Activation Time / Snap Degrees
//
// 与 Java 侧的**不可移植部分**（原因见 docs/telly-port.md）：
//   * eps-plus 直接读游戏内存（LocalPlayer / ItemStack / BlockState）并收发 Packet，
//     本 DLL 架构上不读游戏内存，只做窗口+输入+渲染叠加。
//   * 因此这些设置保留原名与默认值，但标注为「需要 Java 侧」：
//     Auto Swap（要读背包/手持物）、Disable SafeWalk（要能关掉 Java 侧同名模块）、
//     Show Activation Hitbox（要读方块位置画 3D 框）。
//   * 「俯角≥75° 且站在边缘」这一激活前置由玩家自己保证（DLL 无法读视角/世界）；
//     Snap Degrees 的绝对网格吸附同理（绝对 yaw 不可知），用途改为曲线基准角。
// ============================================================================

class Telly : public WindowModule, public UpdateModule, public KeybindModule, public AffixModule, public Item, public SoundModule
{
public:
    Telly()
    {
        type = Util;
        name = u8"搭路(Telly)";
        description = u8"移植自 eps-plus 的脚本 Telly：蹲+看下方+右键触发，自动后退搭桥";
        icon = "o";
        updateIntervalMs = 2;
        lastUpdateTime = std::chrono::steady_clock::now();
        renderTask.after = true;   // 每帧在屏幕上画激活提示
        Telly::Reset();
    }

    static Telly& Instance()
    {
        static Telly instance;
        return instance;
    }

    void Toggle() override;
    void Reset() override;
    void Update() override;
    void HoverSetting() override;
    void DrawContent() override;
    void RenderAfterGui() override;   // 屏幕中心提示（不依赖模块窗口是否显示）
    void DrawSettings(const float& bigPadding, const float& centerX, const float& itemWidth) override;
    void OnKeyEvent(bool state, bool isRepeat, WPARAM key) override;
    void Load(const nlohmann::json& j) override;
    void Save(nlohmann::json& j) const override;

private:
    // ---- 原始曲线（eps-plus Telly.java 逐值复制）----
    static const int   kCurveLen = 21;
    static const int   kSetupTicks = 12;
    static const int   kSetupInertiaTicks = 6;     // SETUP_INERTIA_TICKS
    static const int   kTickMs = 50;               // 20 TPS
    static constexpr float kActivationPitch = 75.0f;   // activationPitch()
    static constexpr float kSetupPitch = 74.52f;       // setup 阶段的旋转目标 pitch

    // ---- 设置（名字/默认值与 Java 侧一致）----
    bool   autoSwap = true;            // "Auto Swap"        （需要 Java 侧）
    bool   disableSafeWalk = true;     // "Disable SafeWalk" （需要 Java 侧）
    bool   showActivationHitbox = false; // "Show Activation Hitbox"（需要 Java 侧）
    bool   print = false;              // "Print"            通知开关
    bool   debugLog = true;            // "Debug Log"
    double activationTime = 1.0;       // "Activation Time"  0.1 ~ 2.0 step 0.1
    double snapBase = 46.5;            // "Snap Degrees"     42.0 ~ 48.0 step 0.1

    // ---- DLL 侧新增（Java 侧由游戏环境提供，这里必须显式给）----
    int    inputMode = (int)InputSim::Mode::SendInput;
    double sensitivity = 0.5;          // 客户端灵敏度，用于「角度 ↔ 鼠标计数」换算
    int    maxRunSeconds = 60;         // 运行上限（DLL 侧没有世界信息，兜底用）

    // ---- 运行状态（与 Java 侧同名同义）----
    bool  armed = false;
    bool  running = false;
    unsigned long long activatePromptAt = 0;
    float promptAlpha = 0.0f;
    unsigned long long promptFadeLastAt = 0;
    int   promptFadeRgb = 0xFF5555;
    int   setupTick = 0;
    int   cyclePhase = 19;
    float baseYaw = 0.0f;

    // ---- 旋转（绝对 pitch / 相对 yaw）----
    bool  rotActive = false;
    unsigned long long rotStartMs = 0;
    unsigned long long rotDurMs = 50;
    float rotStartYaw = 0.0f, rotStartPitch = 0.0f;
    float rotTargetYaw = 0.0f, rotTargetPitch = 0.0f;
    float estYaw = 0.0f;            // 相对 baseYaw 的估计
    float estPitch = 75.0f;         // 绝对 pitch 估计（触发时假定 = kActivationPitch）

    // ---- 输入持有状态 ----
    std::set<int> heldKeys;
    bool useHeld = false;
    unsigned long long lastTickMs = 0;
    unsigned long long runStartMs = 0;

    // ---- 内部函数 ----
    int   sneakVk() const;
    int   gameVk(int action, const char* customName) const;
    std::string vkName(int vk) const;

    double quantum() const;                       // 每 1 鼠标计数 = 多少度
    void  driveTo(float targetYaw, float targetPitch);
    void  setRotationTarget(float targetYaw, float targetPitch, unsigned long long duration);
    void  stepRotation(unsigned long long now);

    void  setHeld(int vk, bool down);
    void  releaseAllKeys();
    void  applyMovement(float forward, float strafe, bool jumping, bool sprinting);
    void  applyUse(bool pressed);

    bool  activationReady() const;
    void  updateActivation(unsigned long long now);
    void  clearActivationPrompt();
    void  rememberPromptColor();
    void  updatePromptFade();

    void  armAutomation();
    void  beginAutomation();
    void  stopAutomation(const char* reason);
    void  runTick(unsigned long long now);

    void  printStatus(const char* prefixed, int type);
    void  dbg(const std::string& line) const;
};