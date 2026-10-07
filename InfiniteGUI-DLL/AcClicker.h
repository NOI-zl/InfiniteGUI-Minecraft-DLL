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
#include <chrono>
#include "KeyState.h"
#include "InputSim.h"

// ============================================================================
// AcClicker —— 把独立版 AC（自动点击器）移植进 InfiniteGUI-DLL
//
// 与独立版一致的语义：
//   * 模式 A「增加点击」：检测到一次真实点击后，自动补 N 次（长按不继续增加）
//   * 模式 B「连点器」：按住左/右键即按该键自己的 CPS 自动连点
//   * 左右键**分别**设置 CPS，各自带抖动
//   * 按住时长（按下→抬起）会被夹在周期之内，保证 CPS 真的是设定值
//   * 状态灯：绿=已启用 / 红=已暂停；可拖动、可调大小（就是本模块的窗口）
//   * 提示、音效、前后缀走本 DLL 既有体系（NotificationItem / SoundModule / AffixModule）
//
// 与独立版的**必要差异**（DLL 里没有 LL 鼠标钩子，无法"吞掉"物理点击）：
//   * 模式 A 不再拦截并伪造那一次点击，而是**在物理点击之后补 N 次**。
//   * 无法 100% 区分"自己注入的点击"与"玩家的物理点击"，用注入时间窗近似
//     （InputSim::InjectedWithin）。注入模式改成 PostMessage 时最干净。
// ============================================================================

class AcClicker : public WindowModule, public UpdateModule, public KeybindModule, public AffixModule, public Item, public SoundModule
{
public:
    AcClicker()
    {
        type = Util;
        name = u8"自动点击(AC)";
        description = u8"自动点击器：模式A补点击 / 模式B长按连点，左右键独立CPS";
        icon = "6";
        updateIntervalMs = 2;
        lastUpdateTime = std::chrono::steady_clock::now();
        AcClicker::Reset();
    }

    static AcClicker& Instance()
    {
        static AcClicker instance;
        return instance;
    }

    void Toggle() override;
    void Reset() override;
    void Update() override;
    void HoverSetting() override;
    void DrawContent() override;
    void DrawSettings(const float& bigPadding, const float& centerX, const float& itemWidth) override;
    void OnKeyEvent(bool state, bool isRepeat, WPARAM key) override;
    void Load(const nlohmann::json& j) override;
    void Save(nlohmann::json& j) const override;

private:
    // ---- 一个按键（左/右）的连点调度状态 ----
    struct ButtonSched
    {
        bool  enabled = true;      // 该键是否参与自动点击
        int   cps = 12;            // 该键的 CPS
        bool  burst = false;       // 模式B：物理按住中，正在连点
        bool  down = false;        // 当前是否有我们发出的按下没抬起
        int   remain = 0;          // 模式A：还要补几次
        unsigned long long next = 0;      // 下一次按下的时间
        unsigned long long upAt = 0;      // 该次按下应抬起的时间
    };

    ButtonSched btn[2];            // 0=左键 1=右键

    int   mode = 0;                // 0=增加点击 1=连点器
    int   extra = 2;               // 模式A 补点次数 N
    int   cpsJitter = 3;           // CPS 抖动 %
    int   holdMin = 12;            // 按住时长下限 ms
    int   holdMax = 34;            // 按住时长上限 ms
    int   longMs = 400;            // 模式A 长按判定 ms（超过就不补点）
    int   inputMode = (int)InputSim::Mode::SendInput;

    bool  showTip = true;          // 启用/暂停时弹提示
    bool  extUseAsClicks = false;  // 外置引擎：把 Telly 的 use 按住转成 CPS 点击
    bool  lampShow = true;         // 显示状态灯
    int   lampSize = 26;           // 灯直径 px
    bool  lampShowText = true;     // 灯下面显示文字状态

    bool  isActivated = true;      // 启用/暂停（状态灯颜色）
    bool  physPrev[2] = { false, false };
    unsigned long long physDownAt[2] = { 0, 0 };
    long  lastClicksDone = 0;      // 外置引擎累计点击数（用于状态灯与脏标记）
    unsigned int rng = 0x9E3779B9u; // 自制 LCG，避免依赖 <random>

    // 辅助
    int   PeriodMs(const ButtonSched& b) const;
    int   HoldFor(const ButtonSched& b, int period) const;
    int   Jitter(int base, int percent, int minimum);
    void  EmitClick(int idx, unsigned long long now);
    void  ReleaseAll();
    void  ReadPhysical(unsigned long long now);
    void  ApplyMode();
    void  SetActivated(bool on);
    void  SetMode(int m);
    void  Notify(const char* text, int type);
};