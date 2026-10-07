#pragma once

#include "AcClicker.h"

#include "Anim.h"
#include "AudioManager.h"
#include "GameKeyBind.h"
#include "GameStateDetector.h"
#include "NotificationItem.h"

// ============================================================================
// 自动点击(AC) —— 独立版 AC 的 DLL 移植
// ============================================================================

void AcClicker::Toggle()
{
    if (!isEnabled) ReleaseAll();
}

void AcClicker::Reset()
{
    ResetWindow();
    ResetKeybind();
    ResetAffix();
    ResetSound();

    isEnabled = false;

    keybinds.insert(std::make_pair(u8"激活键：", VK_F8));
    keybinds.insert(std::make_pair(u8"切换模式键：", VK_F9));

    btn[0] = ButtonSched();  btn[0].enabled = true;  btn[0].cps = 12;
    btn[1] = ButtonSched();  btn[1].enabled = true;  btn[1].cps = 12;

    mode = 0;
    extra = 2;
    cpsJitter = 3;
    holdMin = 12;
    holdMax = 34;
    longMs = 400;
    inputMode = (int)InputSim::Mode::SendInput;
    long lastClicksDone = 0;   // 外置引擎点击数快照
    extUseAsClicks = false;
    showTip = true;
    lampShow = true;
    lampSize = 26;
    lampShowText = true;
    isActivated = true;
    physPrev[0] = physPrev[1] = false;
    physDownAt[0] = physDownAt[1] = 0;
}

// ---------------------------------------------------------------- 时间与抖动

int AcClicker::Jitter(int base, int percent, int minimum)
{
    if (base < minimum) base = minimum;
    if (percent <= 0) return base;
    int span = base * percent / 100;
    if (span < 1) span = 1;
    rng = rng * 1664525u + 1013904223u;
    int delta = (int)(rng % (unsigned int)(span * 2 + 1)) - span;
    int value = base + delta;
    return value < minimum ? minimum : value;
}

int AcClicker::PeriodMs(const ButtonSched& b) const
{
    int cps = b.cps;
    if (cps < 1) cps = 1;
    if (cps > 40) cps = 40;
    int period = (int)(1000.0 / (double)cps + 0.5);
    return period < 6 ? 6 : period;
}

// 按住时长必须夹在周期之内，否则"按住时间"会叠加到周期上，实际 CPS 达不到设定值
int AcClicker::HoldFor(const ButtonSched& b, int period) const
{
    int lo = holdMin < 1 ? 1 : holdMin;
    int hi = holdMax < lo ? lo : holdMax;
    int span = hi - lo;
    int hold = lo;
    if (span > 0)
    {
        rng = rng * 1664525u + 1013904223u;
        hold = lo + (int)(rng % (unsigned int)(span + 1));
    }
    int limit = period - 1;
    if (limit < 1) limit = 1;
    if (hold > limit) hold = limit;
    if (hold < 1) hold = 1;
    return hold;
}

// ---------------------------------------------------------------- 点击产出

void AcClicker::EmitClick(int idx, unsigned long long now)
{
    ButtonSched& b = btn[idx];
    const int vk = (idx == 0) ? VK_LBUTTON : VK_RBUTTON;
    const int period = PeriodMs(b);
    const int hold = HoldFor(b, period);

    InputSim::MouseDown(vk);
    b.down = true;
    b.upAt = now + (unsigned long long)hold;
}

void AcClicker::ReleaseAll()
{
    for (int i = 0; i < 2; ++i)
    {
        if (btn[i].down)
        {
            InputSim::MouseUp((i == 0) ? VK_LBUTTON : VK_RBUTTON);
            btn[i].down = false;
        }
        btn[i].remain = 0;
        btn[i].burst = false;
    }
}

void AcClicker::ReadPhysical(unsigned long long now)
{
    for (int i = 0; i < 2; ++i)
    {
        const int vk = (i == 0) ? VK_LBUTTON : VK_RBUTTON;
        const bool raw = (GetAsyncKeyState(vk) & 0x8000) != 0;

        // 刚注入过的按下不算物理点击（没有 LL 钩子，只能这样近似）
        bool phys = raw;
        if (!raw) phys = false;
        else if (btn[i].down && InputSim::InjectedWithin(vk, 60)) phys = false;

        if (phys && !physPrev[i]) physDownAt[i] = now;

        // ---- 模式A：一次短点 → 补 N 次；长按不补 ----
        if (mode == 0 && btn[i].enabled)
        {
            if (phys && !physPrev[i])
            {
                // 按下：只记时间
            }
            else if (!phys && physPrev[i])
            {
                const unsigned long long held = now - physDownAt[i];
                if (held < (unsigned long long)longMs)
                {
                    btn[i].remain += extra;
                    const unsigned long long gap = now + 20ULL;
                    if (btn[i].next < gap) btn[i].next = gap;
                }
            }
        }

        // ---- 模式B：物理按住 → 连点，松开就停 ----
        if (mode == 1 && btn[i].enabled)
        {
            if (phys && !physPrev[i])
            {
                btn[i].burst = true;
                btn[i].next = now;
            }
            else if (btn[i].burst && !raw && !btn[i].down)
            {
                btn[i].burst = false;   // 玩家松开（我们自己的按下也已抬起）
            }
        }
        else if (mode != 1)
        {
            btn[i].burst = false;
        }

        physPrev[i] = phys;
    }
}

void AcClicker::ApplyMode()
{
    for (int i = 0; i < 2; ++i)
    {
        if (!btn[i].enabled)
        {
            if (btn[i].down) { InputSim::MouseUp((i == 0) ? VK_LBUTTON : VK_RBUTTON); btn[i].down = false; }
            btn[i].remain = 0;
            btn[i].burst = false;
        }
    }
}

// ---------------------------------------------------------------- 状态切换

void AcClicker::Notify(const char* text, int type)
{
    NotificationItem::Instance().AddNotification((NotificationType)type, text);
}

void AcClicker::SetActivated(bool on)
{
    if (isActivated == on) return;
    isActivated = on;
    if (!on) ReleaseAll();

    if (isPlaySound)
        AudioManager::Instance().playSound(on ? "counter\\counter_up.wav" : "counter\\counter_down.wav", soundVolume);

    if (showTip)
        Notify(on ? u8"自动点击：已启用。" : u8"自动点击：已暂停（不再产出点击）。",
               on ? NotificationType_Success : NotificationType_Warning);

    dirtyState.contentDirty = true;
    dirtyState.animating = true;
}

void AcClicker::SetMode(int m)
{
    if (mode == m) return;
    mode = m;
    ReleaseAll();
    if (showTip)
        Notify(m == 0 ? u8"自动点击：模式A 增加点击（1+N）" : u8"自动点击：模式B 连点器（按住自动连点）",
               NotificationType_Info);
    dirtyState.contentDirty = true;
    dirtyState.animating = true;
}

// ---------------------------------------------------------------- 主循环

void AcClicker::OnKeyEvent(bool state, bool isRepeat, WPARAM key)
{
    // 热键在 Update 里轮询（更稳，不受窗口消息转发影响），这里留空实现
}

void AcClicker::Update()
{
    const unsigned long long now = InputSim::NowMs();
    InputSim::SetMode((InputSim::Mode)inputMode);

    // 热键（只在游戏内响应，避免在桌面/浏览器里误触发）
    if (GameStateDetector::Instance().IsInGame())
    {
        if (keyStateHelper.GetKeyClick(keybinds.at(u8"激活键："))) SetActivated(!isActivated);
        if (keyStateHelper.GetKeyClick(keybinds.at(u8"切换模式键："))) SetMode(mode == 0 ? 1 : 0);
    }

    // ---- 外置 AC 引擎：把配置推给外置程序（内部做变更检测）----
    const bool extEngine = (inputMode == (int)InputSim::Mode::External);
    if (extEngine)
    {
        AcInput::AcProfile profile{};
        profile.engineOn = isActivated ? 1u : 0u;   // 暂停要立刻通知外置引擎停手
        profile.mode = (unsigned)mode;
        profile.useAsClicks = extUseAsClicks ? 1u : 0u;
        profile.leftEnabled = btn[0].enabled ? 1u : 0u;
        profile.leftCps = (unsigned)btn[0].cps;
        profile.rightEnabled = btn[1].enabled ? 1u : 0u;
        profile.rightCps = (unsigned)btn[1].cps;
        profile.jitterPct = (unsigned)cpsJitter;
        profile.holdMin = (unsigned)holdMin;
        profile.holdMax = (unsigned)holdMax;
        profile.extra = (unsigned)extra;
        profile.longMs = (unsigned)longMs;
        InputSim::SetAcProfile(profile);
    }

    if (!isActivated)
    {
        ReleaseAll();
        return;
    }

    // ---- 外置 AC 引擎：点击调度搬进外置程序，DLL 侧不再自己发点击 ----
    if (extEngine)
    {
        ReleaseAll();   // 清掉可能残留的本地按下，避免两边同时点

        const long done = InputSim::ExternalClicksDone();
        if (done != lastClicksDone)
        {
            lastClicksDone = done;
            dirtyState.contentDirty = true;
        }
        return;
    }

    ReadPhysical(now);
    ApplyMode();

    for (int i = 0; i < 2; ++i)
    {
        ButtonSched& b = btn[i];
        const int vk = (i == 0) ? VK_LBUTTON : VK_RBUTTON;

        if (b.down && now >= b.upAt)
        {
            InputSim::MouseUp(vk);
            b.down = false;
        }

        if (!b.enabled) continue;

        bool want = (mode == 0) ? (b.remain > 0) : b.burst;
        if (!want || b.down || now < b.next) continue;

        const int period = PeriodMs(b);
        const int jittered = Jitter(period, cpsJitter, 6);
        b.next = now + (unsigned long long)jittered;
        if (mode == 0 && b.remain > 0) b.remain--;

        EmitClick(i, now);
        dirtyState.contentDirty = true;
    }
}

// ---------------------------------------------------------------- 状态灯

void AcClicker::DrawContent()
{
    if (closed)
    {
        isEnabled = false;
        closed = false;
        ReleaseAll();
    }

    if (!lampShow) return;

    float d = (float)lampSize;
    if (d < 8.0f) d = 8.0f;
    if (d > 120.0f) d = 120.0f;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
    float ring = d * 0.09f;
    if (ring < 1.5f) ring = 1.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, d * 0.5f, IM_COL32(35, 35, 35, 255), 48);
    dl->AddCircleFilled(c, d * 0.5f - ring, isActivated ? IM_COL32(38, 200, 96, 255)
                                                       : IM_COL32(226, 54, 54, 255), 48);
    ImGui::Dummy(ImVec2(d, d));

    if (lampShowText)
    {
        ImGuiStd::TextShadow(isActivated ? u8"已启用" : u8"已暂停");
        ImGuiStd::TextShadow(mode == 0 ? u8"模式A 1+N" : u8"模式B 连点");
    }

    dirtyState.animating = false;
}

void AcClicker::HoverSetting()
{
}

// ---------------------------------------------------------------- 设置界面

void AcClicker::DrawSettings(const float& bigPadding, const float& centerX, const float& itemWidth)
{
    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox(u8"启用(绿=启用/红=暂停)", &isActivated);
    if (isActivated && !btn[0].enabled && !btn[1].enabled)
        ImGui::TextDisabled(u8"（左右键都没勾选，不会产出点击）");

    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox(u8"状态提示弹窗", &showTip);
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"启用/暂停、切换模式时在右下角弹提示（走本 DLL 的提示体系）。");

    // ---- 模式 ----
    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    const char* modeNames[] = { u8"模式A 增加点击(1+N)", u8"模式B 连点器(长按连点)" };
    int modeSel = mode;
    if (ImGui::Combo(u8"功能模式", &modeSel, modeNames, IM_ARRAYSIZE(modeNames)))
        SetMode(modeSel);
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"模式A：一次短点自动补 N 次，长按不增加。\n模式B：按住左/右键按该键的 CPS 自动连点。");

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"补点次数 N", &extra, 1, 20);

    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"长按判定 ms", &longMs, 100, 1200);

    // ---- 左右键 CPS ----
    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox(u8"左键参与", &btn[0].enabled);
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"左键 CPS", &btn[0].cps, 1, 40);

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox(u8"右键参与", &btn[1].enabled);
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"右键 CPS", &btn[1].cps, 1, 40);

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"CPS 抖动 %", &cpsJitter, 0, 30);
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"按住时长下限 ms", &holdMin, 1, 200);

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"按住时长上限 ms", &holdMax, 1, 300);
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"按住时长会被自动夹进点击周期内，保证实际 CPS 等于设定值。");

    // ---- 状态灯 ----
    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox(u8"显示状态灯", &lampShow);
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"灯直径 px", &lampSize, 8, 120);

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox(u8"灯下显示文字", &lampShowText);
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    const char* inputModeNames[] = {
        u8"模拟输入(SendInput)",
        u8"按键事件(MouseEvent)",
        u8"发送消息(PostMessage)",
        u8"外置注入(隐藏/绕过)"
    };
    ImGui::Combo(u8"注入模式", &inputMode, inputModeNames, IM_ARRAYSIZE(inputModeNames));
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"外置注入：点击交给外部 ACTGInjector，用内核驱动/外置硬件发出，\n"
                         u8"不带 LLMHF_INJECTED 标记，游戏与反作弊看不出是注入的输入。");
    if (inputMode == (int)InputSim::Mode::External)
    {
        ImGui::SetCursorPosX(bigPadding);
        ImGui::TextDisabled(u8"外置注入器：%s", InputSim::ExternalBackendName());
        if (!InputSim::ExternalConnected())
        {
            ImGui::SameLine();
            ImGuiStd::TextColoredShadow(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), u8"（未连接）");
        }

        ImGui::SetCursorPosX(bigPadding);
        ImGui::SetNextItemWidth(itemWidth);
        ImGui::Checkbox(u8"放置走外置AC（use转连点）", &extUseAsClicks);
        ImGui::SameLine();
        ImGui::SetCursorPosX(centerX + bigPadding);
        ImGui::TextDisabled(u8"外置点击 %ld", lastClicksDone);
        ImGui::SameLine();
        ImGuiStd::HelpMarker(u8"勾上：搭路/蹲起塔的放置不再“一直按住右键”，而是按右键 CPS 转成连点，\n"
                             u8"全部由外置程序用隐藏后端发出（也就是用外置的 AC 来点）。\n"
                             u8"不勾：保持原版语义——按住右键，但同样走外置隐藏通道。");
    }

    DrawKeybindSettings(bigPadding, centerX, itemWidth);
    DrawAffixSettings(bigPadding, centerX, itemWidth);
    DrawSoundSettings(bigPadding, centerX, itemWidth);
    DrawWindowSettings(bigPadding, centerX, itemWidth);
}

// ---------------------------------------------------------------- 配置

void AcClicker::Load(const nlohmann::json& j)
{
    LoadItem(j);
    LoadWindow(j);
    LoadKeybind(j);
    LoadAffix(j);
    LoadSound(j);

    if (j.contains("mode")) mode = j["mode"];
    if (j.contains("extra")) extra = j["extra"];
    if (j.contains("cpsJitter")) cpsJitter = j["cpsJitter"];
    if (j.contains("holdMin")) holdMin = j["holdMin"];
    if (j.contains("holdMax")) holdMax = j["holdMax"];
    if (j.contains("longMs")) longMs = j["longMs"];
    if (j.contains("inputMode")) inputMode = j["inputMode"];
    if (j.contains("showTip")) showTip = j["showTip"];
    if (j.contains("extUseAsClicks")) extUseAsClicks = j["extUseAsClicks"];
    if (j.contains("lampShow")) lampShow = j["lampShow"];
    if (j.contains("lampSize")) lampSize = j["lampSize"];
    if (j.contains("lampShowText")) lampShowText = j["lampShowText"];
    if (j.contains("isActivated")) isActivated = j["isActivated"];
    if (j.contains("leftEnabled")) btn[0].enabled = j["leftEnabled"];
    if (j.contains("leftCps")) btn[0].cps = j["leftCps"];
    if (j.contains("rightEnabled")) btn[1].enabled = j["rightEnabled"];
    if (j.contains("rightCps")) btn[1].cps = j["rightCps"];
}

void AcClicker::Save(nlohmann::json& j) const
{
    SaveItem(j);
    SaveWindow(j);
    SaveKeybind(j);
    SaveAffix(j);
    SaveSound(j);

    j["mode"] = mode;
    j["extra"] = extra;
    j["cpsJitter"] = cpsJitter;
    j["holdMin"] = holdMin;
    j["holdMax"] = holdMax;
    j["longMs"] = longMs;
    j["inputMode"] = inputMode;
    j["showTip"] = showTip;
    j["extUseAsClicks"] = extUseAsClicks;
    j["lampShow"] = lampShow;
    j["lampSize"] = lampSize;
    j["lampShowText"] = lampShowText;
    j["isActivated"] = isActivated;
    j["leftEnabled"] = btn[0].enabled;
    j["leftCps"] = btn[0].cps;
    j["rightEnabled"] = btn[1].enabled;
    j["rightCps"] = btn[1].cps;
}