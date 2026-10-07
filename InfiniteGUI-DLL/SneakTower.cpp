#pragma once

#include "SneakTower.h"

#include "AudioManager.h"
#include "GameKeyBind.h"
#include "GameStateDetector.h"
#include "NotificationItem.h"

#include <cstdio>

// ---------------------------------------------------------------- 生命周期

void SneakTower::Toggle()
{
    if (!isEnabled) setActive(false);
}

void SneakTower::Reset()
{
    ResetWindow();
    ResetKeybind();
    ResetAffix();
    ResetSound();

    isEnabled = false;

    keybinds.insert(std::make_pair(u8"蹲起塔键：", 'Z'));

    gameKeybinds.insert(std::make_pair(u8"跳跃键：", VK_SPACE));
    gameKeybinds.insert(std::make_pair(u8"潜行键：", VK_SHIFT));

    holdMode = false;
    cycleGapMs = 250;
    jumpHoldMs = 80;
    sneakHoldMs = 90;
    placeHoldMs = 60;
    sneakDelayMs = 60;
    placeDelayMs = 60;
    maxCycles = 0;
    inputMode = (int)InputSim::Mode::SendInput;

    active = false;
    step = Step::Idle;
    stepAt = 0;
    cycle = 0;
    jumpHeld = false;
    sneakHeld = false;
    placeHeld = false;
}

// ---------------------------------------------------------------- 键位

int SneakTower::gameVk(int action, const char* customName) const
{
    if (customGameKeybinds)
    {
        auto it = gameKeybinds.find(customName);
        if (it != gameKeybinds.end()) return it->second;
    }
    return GameKeyBind::Instance().GetVK((GameAction)action);
}

int SneakTower::sneakVk() const
{
    return customGameKeybinds ? gameKeybinds.at(u8"潜行键：")
                              : GameKeyBind::Instance().GetVK(GameAction::Sneak);
}

int SneakTower::jumpVk() const
{
    return customGameKeybinds ? gameKeybinds.at(u8"跳跃键：")
                              : GameKeyBind::Instance().GetVK(GameAction::Jump);
}

// ---------------------------------------------------------------- 状态

void SneakTower::setActive(bool on)
{
    if (active == on) return;
    active = on;

    if (!on) releaseAll();
    else
    {
        cycle = 0;
        step = Step::Idle;
        stepAt = InputSim::NowMs();
    }

    if (isPlaySound)
        AudioManager::Instance().playSound(on ? "counter\\counter_up.wav" : "counter\\counter_down.wav", soundVolume);

    NotificationItem::Instance().AddNotification(
        on ? NotificationType_Success : NotificationType_Warning,
        on ? u8"蹲起塔：已开启。" : u8"蹲起塔：已关闭。");

    dirtyState.contentDirty = true;
    dirtyState.animating = true;
}

void SneakTower::releaseAll()
{
    if (jumpHeld)  { InputSim::KeyUp(jumpVk());  jumpHeld = false; }
    if (sneakHeld) { InputSim::KeyUp(sneakVk()); sneakHeld = false; }
    if (placeHeld) { InputSim::MouseUp(VK_RBUTTON); placeHeld = false; }
    step = Step::Idle;
    stepAt = InputSim::NowMs();
}

void SneakTower::enterStep(Step next, unsigned long long now)
{
    step = next;
    stepAt = now;
}

// ---------------------------------------------------------------- 蹲起循环

void SneakTower::runCycle(unsigned long long now)
{
    switch (step)
    {
    case Step::Idle:
        if (maxCycles > 0 && cycle >= maxCycles)
        {
            setActive(false);
            return;
        }
        enterStep(Step::Jump, now);
        break;

    case Step::Jump:
        if (!jumpHeld) { InputSim::KeyDown(jumpVk()); jumpHeld = true; }
        if (now - stepAt >= (unsigned long long)jumpHoldMs)
        {
            InputSim::KeyUp(jumpVk());
            jumpHeld = false;
            enterStep(Step::Sneak, now + (unsigned long long)sneakDelayMs);
        }
        break;

    case Step::Sneak:
        if (now < stepAt) break;
        if (!sneakHeld) { InputSim::KeyDown(sneakVk()); sneakHeld = true; }
        if (now - stepAt >= (unsigned long long)sneakHoldMs)
        {
            InputSim::KeyUp(sneakVk());
            sneakHeld = false;
            enterStep(Step::Place, now + (unsigned long long)placeDelayMs);
        }
        break;

    case Step::Place:
        if (now < stepAt) break;
        if (!placeHeld)
        {
            // 放置统一走 InputSim：本地模式=直接右键；外置模式=交给外置 AC 用隐藏后端发出
            InputSim::MouseDown(VK_RBUTTON);
            placeHeld = true;
        }
        if (now - stepAt >= (unsigned long long)placeHoldMs)
        {
            InputSim::MouseUp(VK_RBUTTON);
            placeHeld = false;
            ++cycle;
            enterStep(Step::Idle, now + (unsigned long long)cycleGapMs);
            dirtyState.contentDirty = true;
        }
        break;

    default:
        enterStep(Step::Idle, now);
        break;
    }
}

// ---------------------------------------------------------------- 主循环

void SneakTower::OnKeyEvent(bool state, bool isRepeat, WPARAM key)
{
}

void SneakTower::Update()
{
    const unsigned long long now = InputSim::NowMs();
    InputSim::SetMode((InputSim::Mode)inputMode);

    const int vk = keybinds.at(u8"蹲起塔键：");

    if (holdMode)
    {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down != active) setActive(down);
    }
    else
    {
        if (keyStateHelper.GetKeyClick(vk)) setActive(!active);
    }

    if (!active) return;

    if (!GameStateDetector::Instance().IsInGame())
    {
        setActive(false);   // 开背包/菜单/聊天就停，避免乱按键
        return;
    }

    runCycle(now);
}

// ---------------------------------------------------------------- 绘制

void SneakTower::DrawContent()
{
    if (closed)
    {
        isEnabled = false;
        closed = false;
        setActive(false);
    }

    if (active)
        ImGuiStd::TextColoredShadow(ImVec4(0.1f, 1.0f, 0.1f, 1.0f), u8"蹲起中");
    else
        ImGuiStd::TextShadow(u8"待触发");

    char buf[96];
    sprintf_s(buf, u8"已完成 %d 次", cycle);
    ImGuiStd::TextShadow(buf);

    dirtyState.animating = false;
}

void SneakTower::HoverSetting()
{
}

// ---------------------------------------------------------------- 设置界面

void SneakTower::DrawSettings(const float& bigPadding, const float& centerX, const float& itemWidth)
{
    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox(u8"按住触发(不勾=按一下开关)", &holdMode);

    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"周期间隔 ms", &cycleGapMs, 50, 2000);

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"跳跃按住 ms", &jumpHoldMs, 20, 500);
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"潜行脉冲 ms", &sneakHoldMs, 30, 500);
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"必须大于 1 个游戏 tick（50ms），否则不会重置坠落距离，塔会掉。");

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"放置按住 ms", &placeHoldMs, 20, 500);
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"跳起→潜行 延迟 ms", &sneakDelayMs, 0, 400);

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"潜行→放置 延迟 ms", &placeDelayMs, 0, 400);
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"最多次数(0=不限)", &maxCycles, 0, 200);

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    const char* inputModeNames[] = {
        u8"模拟输入(SendInput)",
        u8"按键事件(MouseEvent)",
        u8"发送消息(PostMessage)",
        u8"外置注入(隐藏/绕过)"
    };
    ImGui::Combo(u8"注入模式", &inputMode, inputModeNames, IM_ARRAYSIZE(inputModeNames));

    DrawKeybindSettings(bigPadding, centerX, itemWidth);
    DrawAffixSettings(bigPadding, centerX, itemWidth);
    DrawSoundSettings(bigPadding, centerX, itemWidth);
    DrawWindowSettings(bigPadding, centerX, itemWidth);
}

// ---------------------------------------------------------------- 配置

void SneakTower::Load(const nlohmann::json& j)
{
    LoadItem(j);
    LoadWindow(j);
    LoadKeybind(j);
    LoadAffix(j);
    LoadSound(j);

    if (j.contains("holdMode")) holdMode = j["holdMode"];
    if (j.contains("cycleGapMs")) cycleGapMs = j["cycleGapMs"];
    if (j.contains("jumpHoldMs")) jumpHoldMs = j["jumpHoldMs"];
    if (j.contains("sneakHoldMs")) sneakHoldMs = j["sneakHoldMs"];
    if (j.contains("placeHoldMs")) placeHoldMs = j["placeHoldMs"];
    if (j.contains("sneakDelayMs")) sneakDelayMs = j["sneakDelayMs"];
    if (j.contains("placeDelayMs")) placeDelayMs = j["placeDelayMs"];
    if (j.contains("maxCycles")) maxCycles = j["maxCycles"];
    if (j.contains("inputMode")) inputMode = j["inputMode"];
}

void SneakTower::Save(nlohmann::json& j) const
{
    SaveItem(j);
    SaveWindow(j);
    SaveKeybind(j);
    SaveAffix(j);
    SaveSound(j);

    j["holdMode"] = holdMode;
    j["cycleGapMs"] = cycleGapMs;
    j["jumpHoldMs"] = jumpHoldMs;
    j["sneakHoldMs"] = sneakHoldMs;
    j["placeHoldMs"] = placeHoldMs;
    j["sneakDelayMs"] = sneakDelayMs;
    j["placeDelayMs"] = placeDelayMs;
    j["maxCycles"] = maxCycles;
    j["inputMode"] = inputMode;
}