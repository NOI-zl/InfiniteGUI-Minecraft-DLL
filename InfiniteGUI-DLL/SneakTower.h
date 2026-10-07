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
// SneakTower（蹲起塔）—— 单塔/速塔用的「蹲起」循环
//
// 一个周期的动作顺序（都由本模块自己注入，不依赖玩家按住鼠标）：
//     跳跃 → 上升途中潜行脉冲（重置坠落距离）→ 右键放置 → 等待间隔 → 重复
//
// 注意：触发键**不能是鼠标右键**。玩家的物理右键按住时，游戏看到的是一个
// 连续的按下状态，注入的 down/up 无法形成新的"点击沿"，因而不会重复放置。
// 所以这里用独立热键（默认 Z）开关，右键由模块自己按下/抬起。
// ============================================================================

class SneakTower : public WindowModule, public UpdateModule, public KeybindModule, public AffixModule, public Item, public SoundModule
{
public:
    SneakTower()
    {
        type = Util;
        name = u8"蹲起塔";
        description = u8"蹲起循环：跳跃→潜行脉冲→右键放置，自动起塔";
        icon = "o";
        updateIntervalMs = 2;
        lastUpdateTime = std::chrono::steady_clock::now();
        SneakTower::Reset();
    }

    static SneakTower& Instance()
    {
        static SneakTower instance;
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
    enum class Step
    {
        Idle = 0,
        Jump,        // 按下跳跃键
        JumpUp,      // 抬起跳跃键
        Sneak,       // 潜行按下（重置坠落距离）
        SneakUp,     // 潜行抬起
        Place,       // 右键按下（放置）
        PlaceUp,     // 右键抬起
        Wait         // 周期间隔
    };

    // ---- 设置 ----
    bool  holdMode = false;      // true=按住作弊键持续，false=按一下开/关
    int   cycleGapMs = 250;      // 周期间隔
    int   jumpHoldMs = 80;       // 跳跃键按住时长
    int   sneakHoldMs = 90;      // 潜行脉冲时长（要 > 1 tick，50ms，否则不重置坠落距离）
    int   placeHoldMs = 60;      // 右键按住时长
    int   sneakDelayMs = 60;     // 跳起后到潜行脉冲的延迟
    int   placeDelayMs = 60;     // 潜行后到放置的延迟
    int   maxCycles = 0;         // 0=不限
    int   inputMode = (int)InputSim::Mode::SendInput;

    // ---- 运行状态 ----
    bool  active = false;
    Step  step = Step::Idle;
    unsigned long long stepAt = 0;
    int   cycle = 0;
    bool  jumpHeld = false;
    bool  sneakHeld = false;
    bool  placeHeld = false;

    // ---- 内部 ----
    int   gameVk(int action, const char* customName) const;
    int   sneakVk() const;
    int   jumpVk() const;
    void  enterStep(Step next, unsigned long long now);
    void  runCycle(unsigned long long now);
    void  releaseAll();
    void  setActive(bool on);
};