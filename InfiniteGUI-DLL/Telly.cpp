#pragma once

#include "Telly.h"

#include "AudioManager.h"
#include "GameKeyBind.h"
#include "GameStateDetector.h"
#include "NotificationItem.h"

#include <cmath>
#include <cstdio>

// ============================================================================
// 21 帧曲线：逐值复制自 eps-plus 26.2.x
//   common/src/main/java/com/github/epsilon/modules/impl/movement/Telly.java
// 改动任何一个数字都会让"行为变"，所以这里原样保留。
// ============================================================================
static const float YAW_CURVE[21] = {
    91.68f, 98.88f, 78.94f, 37.45f, 1.61f, -21.69f, -33.98f,
    -35.80f, -34.64f, -33.85f, -33.06f, -31.55f, -29.26f, -26.65f,
    -24.19f, -21.07f, -18.84f, -17.06f, -8.87f, 2.61f, 41.94f
};

static const float PITCH_CURVE[21] = {
    64.31f, 59.95f, 60.57f, 61.46f, 60.64f, 58.89f, 56.91f,
    56.63f, 58.65f, 61.63f, 64.20f, 66.74f, 68.69f, 70.64f,
    73.01f, 75.37f, 77.46f, 78.56f, 78.90f, 77.22f, 72.25f
};

static const float FORWARD_CURVE[21] = {
    1.0f, 1.0f, 0.0f, 0.0f, -1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 1.0f
};

static const float STRAFE_CURVE[21] = {
    -1.0f, -1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, -1.0f, -1.0f, -1.0f, -1.0f
};

// ---------------------------------------------------------------- 生命周期

void Telly::Toggle()
{
    if (!isEnabled) stopAutomation("module off");
}

void Telly::Reset()
{
    ResetWindow();
    ResetKeybind();
    ResetAffix();
    ResetSound();

    isEnabled = false;

    keybinds.insert(std::make_pair(u8"激活/停止键：", 'X'));

    gameKeybinds.insert(std::make_pair(u8"前进键：", 'W'));
    gameKeybinds.insert(std::make_pair(u8"后退键：", 'S'));
    gameKeybinds.insert(std::make_pair(u8"左移键：", 'A'));
    gameKeybinds.insert(std::make_pair(u8"右移键：", 'D'));
    gameKeybinds.insert(std::make_pair(u8"跳跃键：", VK_SPACE));
    gameKeybinds.insert(std::make_pair(u8"潜行键：", VK_SHIFT));
    gameKeybinds.insert(std::make_pair(u8"疾跑键：", VK_CONTROL));

    autoSwap = true;
    disableSafeWalk = true;
    showActivationHitbox = false;
    print = false;
    debugLog = true;
    activationTime = 1.0;
    snapBase = 46.5;
    inputMode = (int)InputSim::Mode::SendInput;
    sensitivity = 0.5;
    maxRunSeconds = 60;

    armed = true;
    running = false;
    activatePromptAt = 0;
    promptAlpha = 0.0f;
    promptFadeLastAt = 0;
    promptFadeRgb = 0xFF5555;
    setupTick = 0;
    cyclePhase = 19;
    baseYaw = 0.0f;
    rotActive = false;
    rotDurMs = 50;
    estYaw = 0.0f;
    estPitch = kActivationPitch;
    heldKeys.clear();
    useHeld = false;
    lastTickMs = 0;
    runStartMs = 0;

    dirtyState.contentDirty = true;
    dirtyState.animating = true;
}

// ---------------------------------------------------------------- 按键名/映射

int Telly::sneakVk() const
{
    return customGameKeybinds ? gameKeybinds.at(u8"潜行键：")
                              : GameKeyBind::Instance().GetVK(GameAction::Sneak);
}

int Telly::gameVk(int action, const char* customName) const
{
    if (customGameKeybinds)
    {
        auto it = gameKeybinds.find(customName);
        if (it != gameKeybinds.end()) return it->second;
    }
    return GameKeyBind::Instance().GetVK((GameAction)action);
}

std::string Telly::vkName(int vk) const
{
    switch (vk)
    {
    case VK_LBUTTON:  return u8"左键";
    case VK_RBUTTON:  return u8"右键";
    case VK_MBUTTON:  return u8"中键";
    case VK_SHIFT: case VK_LSHIFT: return "Shift";
    case VK_RSHIFT:   return "RShift";
    case VK_CONTROL: case VK_LCONTROL: return "Ctrl";
    case VK_RCONTROL: return "RCtrl";
    case VK_MENU: case VK_LMENU: return "Alt";
    case VK_SPACE:    return "Space";
    case VK_TAB:      return "Tab";
    case VK_CAPITAL:  return "CapsLock";
    case VK_ESCAPE:   return "Esc";
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        char buf[8];
        sprintf_s(buf, "F%d", vk - VK_F1 + 1);
        return buf;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
    {
        char buf[2] = { (char)vk, 0 };
        return buf;
    }
    char buf[16];
    sprintf_s(buf, "VK 0x%02X", vk);
    return buf;
}

// ---------------------------------------------------------------- 旋转驱动

double Telly::quantum() const
{
    // 原版（1.13+）鼠标灵敏度换算：每 1 个鼠标计数 =
    //   (sens * 0.6 + 0.2)^3 * 8 * 0.15 度
    double s = sensitivity;
    if (s < 0.0) s = 0.0;
    if (s > 1.0) s = 1.0;
    double f = s * 0.6 + 0.2;
    f = f * f * f * 8.0;
    return f * 0.15;
}

void Telly::driveTo(float targetYaw, float targetPitch)
{
    const double q = quantum();
    if (q <= 0.00001) return;

    int dx = (int)std::lround(((double)targetYaw - (double)estYaw) / q);
    int dy = (int)std::lround(((double)targetPitch - (double)estPitch) / q);

    // 单帧上限：避免瞬移（真人不会一帧甩几十度），也压低反作弊特征
    const int kMaxPerStep = 40;
    if (dx >  kMaxPerStep) dx =  kMaxPerStep;
    if (dx < -kMaxPerStep) dx = -kMaxPerStep;
    if (dy >  kMaxPerStep) dy =  kMaxPerStep;
    if (dy < -kMaxPerStep) dy = -kMaxPerStep;

    if (dx == 0 && dy == 0) return;

    InputSim::MouseMoveRel(dx, dy);
    estYaw += (float)(dx * q);
    estPitch += (float)(dy * q);
}

void Telly::setRotationTarget(float targetYaw, float targetPitch, unsigned long long duration)
{
    // 与 Java 侧一致：先结算上一段插值，再以"当前值"为新起点，50ms 平滑推入
    stepRotation(InputSim::NowMs());
    rotStartYaw = (rotActive ? (rotStartYaw + (rotTargetYaw - rotStartYaw) * 0.0f) : estYaw);
    rotStartYaw = estYaw;
    rotStartPitch = estPitch;
    rotTargetYaw = targetYaw;
    rotTargetPitch = targetPitch;
    rotDurMs = (duration == 0 ? 50 : duration);
    rotStartMs = InputSim::NowMs();
    rotActive = true;
}

void Telly::stepRotation(unsigned long long now)
{
    if (!rotActive) return;

    float t = (float)((double)(now - rotStartMs) / (double)rotDurMs);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    const float yaw = rotStartYaw + (rotTargetYaw - rotStartYaw) * t;
    const float pitch = rotStartPitch + (rotTargetPitch - rotStartPitch) * t;
    driveTo(yaw, pitch);

    if (t >= 1.0f) rotActive = false;
}

// ---------------------------------------------------------------- 输入持有

void Telly::setHeld(int vk, bool down)
{
    if (vk <= 0) return;

    if (down)
    {
        if (heldKeys.find(vk) == heldKeys.end())
        {
            InputSim::KeyDown(vk);
            heldKeys.insert(vk);
        }
        return;
    }

    auto it = heldKeys.find(vk);
    if (it == heldKeys.end()) return;

    // 玩家自己也按着同一个键时不强行抬起（DLL 无法拦截物理按键）
    if (!(GetAsyncKeyState(vk) & 0x8000)) InputSim::KeyUp(vk);
    heldKeys.erase(it);
}

void Telly::releaseAllKeys()
{
    for (int vk : heldKeys)
    {
        if (!(GetAsyncKeyState(vk) & 0x8000)) InputSim::KeyUp(vk);
    }
    heldKeys.clear();
}

void Telly::applyMovement(float forward, float strafe, bool jumping, bool sprinting)
{
    setHeld(gameVk((int)GameAction::Forward, u8"前进键："), forward > 0.01f);
    setHeld(gameVk((int)GameAction::Back,    u8"后退键："), forward < -0.01f);
    setHeld(gameVk((int)GameAction::Right,   u8"右移键："), strafe > 0.01f);
    setHeld(gameVk((int)GameAction::Left,    u8"左移键："), strafe < -0.01f);
    setHeld(gameVk((int)GameAction::Jump,    u8"跳跃键："), jumping);
    setHeld(gameVk((int)GameAction::Sprint,  u8"疾跑键："), sprinting);
}

void Telly::applyUse(bool pressed)
{
    if (pressed == useHeld) return;

    // 放置（use）统一走 InputSim::UseHold：
    //   本地注入模式 -> 直接按住/抬起右键（与原版 applyUse 语义一致）
    //   外置注入模式 -> 交给外置程序，由它决定"按住"还是转成按 CPS 的点击
    //                   （也就是让搭路的放置走外置 AC 的隐藏通道）
    InputSim::UseHold(pressed);
    useHeld = pressed;
}

// ---------------------------------------------------------------- 激活手势

bool Telly::activationReady() const
{
    if (activatePromptAt == 0) return false;
    const unsigned long long need = (unsigned long long)(activationTime * 1000.0);
    return (InputSim::NowMs() - activatePromptAt) >= need;
}

void Telly::rememberPromptColor()
{
    if (activatePromptAt == 0) return;
    promptFadeRgb = activationReady() ? 0x55FF55 : 0xFF5555;
}

void Telly::clearActivationPrompt()
{
    rememberPromptColor();
    activatePromptAt = 0;
}

void Telly::updateActivation(unsigned long long now)
{
    if (!GameStateDetector::Instance().IsInGame())
    {
        clearActivationPrompt();
        return;
    }

    const int sneak = sneakVk();
    const bool sneakDown = (sneak > 0) && ((GetAsyncKeyState(sneak) & 0x8000) != 0);
    const bool useDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    if (!sneakDown)
    {
        clearActivationPrompt();
        return;
    }

    if (activatePromptAt == 0)
    {
        activatePromptAt = now;
        dbg("ARM sneak-start yawEst=" + std::to_string(estYaw));
    }

    // 计时就绪 + 按住右键 = 触发（提示此时已由红转绿）
    if (activationReady() && useDown)
    {
        beginAutomation();
    }
}

void Telly::updatePromptFade()
{
    const bool show = armed && !running && activatePromptAt != 0;
    if (show) rememberPromptColor();

    const unsigned long long now = InputSim::NowMs();
    unsigned long long elapsed = (promptFadeLastAt == 0) ? 0 : (now - promptFadeLastAt);
    if (elapsed > 100ULL) elapsed = 100ULL;
    promptFadeLastAt = now;

    const float step = (float)elapsed / 200.0f;
    promptAlpha += show ? step : -step;
    if (promptAlpha < 0.0f) promptAlpha = 0.0f;
    if (promptAlpha > 1.0f) promptAlpha = 1.0f;
}

// ---------------------------------------------------------------- 运行控制

void Telly::printStatus(const char* text, int type)
{
    if (!print) return;
    // Java 侧的通知标题是 "Telly"；本 DLL 的提示弹窗按类型自动取标题，
    // 所以把 "Telly: " 并进正文，保证文字本身与原版一致。
    NotificationItem::Instance().AddNotification((NotificationType)type,
        std::string("Telly: ") + text);
}

void Telly::dbg(const std::string& line) const
{
    if (!debugLog) return;
    OutputDebugStringA(("[Telly] " + line + "\n").c_str());
}

void Telly::armAutomation()
{
    armed = true;
    running = false;
    activatePromptAt = 0;
    setupTick = 0;
    cyclePhase = 19;
    rotActive = false;
    estYaw = 0.0f;
    estPitch = kActivationPitch;
    dirtyState.contentDirty = true;

    printStatus((std::string("Armed. Hold ") + vkName(sneakVk())
        + " at the edge looking down; when green hold " + vkName(VK_RBUTTON)).c_str(),
        NotificationType_Warning);
}

void Telly::beginAutomation()
{
    // Java 侧在这里检查"手持物是不是方块"和"yaw 是否对齐网格"；
    // 两者都需要读游戏内存，DLL 侧无法判断，改为由玩家保证（见 docs/telly-port.md）。
    baseYaw = 0.0f;                 // 以触发瞬间的朝向为基准（相对零点）
    estYaw = 0.0f;
    estPitch = kActivationPitch;    // 激活判据就是俯角 75°，以此作为 pitch 绝对基准
    setupTick = 0;
    cyclePhase = 19;
    armed = false;
    running = true;
    promptAlpha = 0.0f;
    activatePromptAt = 0;
    rotActive = false;
    lastTickMs = InputSim::NowMs();
    runStartMs = lastTickMs;

    applyMovement(-1.0f, -1.0f, false, false);
    setRotationTarget(baseYaw, kSetupPitch, 50);
    applyUse(true);

    if (isPlaySound) AudioManager::Instance().playSound("counter\\counter_up.wav", soundVolume);
    printStatus("Started", NotificationType_Success);
    dbg("BEGIN yawEst=0.00 pitchEst=75.00");
}

void Telly::stopAutomation(const char* reason)
{
    if (running)
    {
        dbg(std::string("STOP reason=") + reason
            + " phase=" + std::to_string(cyclePhase)
            + " setupTick=" + std::to_string(setupTick));
    }

    running = false;
    armed = true;
    setupTick = 0;
    cyclePhase = 19;
    rotActive = false;
    estYaw = 0.0f;
    estPitch = kActivationPitch;
    activatePromptAt = 0;

    releaseAllKeys();
    applyUse(false);

    if (isPlaySound) AudioManager::Instance().playSound("counter\\counter_down.wav", soundVolume);
    printStatus((std::string("Stopped. Hold ") + vkName(sneakVk())
        + " looking down to arm again").c_str(), NotificationType_Warning);

    dirtyState.contentDirty = true;
}

void Telly::runTick(unsigned long long now)
{
    // ---- 停止条件（DLL 侧可判断的部分）----
    if (maxRunSeconds > 0 && (now - runStartMs) > (unsigned long long)maxRunSeconds * 1000ULL)
    {
        stopAutomation("timeout");
        return;
    }
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)   // 运行中左键 = 攻击 = 立即停（Java 侧同义）
    {
        stopAutomation("attack");
        return;
    }
    if (!GameStateDetector::Instance().IsInGame())   // 开背包/菜单/聊天
    {
        stopAutomation("menu");
        return;
    }

    // ---- 固定 50ms 一 tick（对应游戏 20 TPS）----
    if (now - lastTickMs < (unsigned long long)kTickMs) return;
    lastTickMs += (unsigned long long)kTickMs;
    if (lastTickMs + 200ULL < now) lastTickMs = now;

    // ---- setup 阶段：12 tick ----
    if (setupTick >= 0)
    {
        if (setupTick < kSetupTicks)
        {
            const bool setupJump = setupTick >= kSetupInertiaTicks;
            applyMovement(-1.0f, -1.0f, setupJump, false);
            applyUse(true);

            if (setupTick == kSetupTicks - 1)
                setRotationTarget(baseYaw + YAW_CURVE[19], PITCH_CURVE[19], 50);
            else
                setRotationTarget(baseYaw, kSetupPitch, 50);

            ++setupTick;
            return;
        }
        setupTick = -1;
        cyclePhase = 19;
    }

    // ---- 21 帧循环 ----
    const int phase = cyclePhase;
    const float strafe = STRAFE_CURVE[phase];
    const bool sprinting = (phase == 0 || phase == 1);
    const bool jumping = (phase >= 1 && phase <= 19);
    const bool use = (phase >= 7);

    applyMovement(FORWARD_CURVE[phase], strafe, jumping, sprinting);
    applyUse(use);

    const int nextPhase = (phase + 1) % kCurveLen;
    setRotationTarget(baseYaw + YAW_CURVE[nextPhase], PITCH_CURVE[nextPhase], 50);
    cyclePhase = nextPhase;

    dirtyState.contentDirty = true;
    dirtyState.animating = false;
}

// ---------------------------------------------------------------- 主循环

void Telly::OnKeyEvent(bool state, bool isRepeat, WPARAM key)
{
    // 热键轮询在 Update 里做
}

void Telly::Update()
{
    const unsigned long long now = InputSim::NowMs();
    InputSim::SetMode((InputSim::Mode)inputMode);

    // 激活/停止键（默认 X）：随时可停，保证不会失控
    if (keyStateHelper.GetKeyClick(keybinds.at(u8"激活/停止键：")))
    {
        if (running) stopAutomation("hotkey");
        else armAutomation();
    }

    if (running)
    {
        runTick(now);
        stepRotation(now);
        return;
    }

    updateActivation(now);
}

// ---------------------------------------------------------------- 绘制

void Telly::RenderAfterGui()
{
    WindowModule::RenderAfterGui();

    // 开了背包/菜单/聊天时不要糊在界面上
    if (GameStateDetector::Instance().IsNeedHide()) return;

    updatePromptFade();

    if (promptAlpha <= 0.001f) return;

    // 文案与原版一致：
    //   未就绪 -> "Hold <潜行键>"                    红 0xFF5555
    //   就绪   -> "<潜行键> + <右键>"                 绿 0x55FF55
    const std::string text = activationReady()
        ? (vkName(sneakVk()) + " + " + vkName(VK_RBUTTON))
        : ("Hold " + vkName(sneakVk()));

    int alpha = (int)(promptAlpha * 255.0f);
    if (alpha < 16) alpha = 16;
    const ImU32 color = IM_COL32((promptFadeRgb >> 16) & 0xFF, (promptFadeRgb >> 8) & 0xFF,
                                 promptFadeRgb & 0xFF, alpha);

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pos(io.DisplaySize.x * 0.5f - size.x * 0.5f, io.DisplaySize.y * 0.5f + 10.0f);

    ImGui::GetForegroundDrawList()->AddText(pos, color, text.c_str());

    // 诊断行：与原版同位置（屏幕底部居中、白色）
    if (debugLog && running)
    {
        char line[160];
        sprintf_s(line, "Telly phase=%d setup=%d yawEst=%.1f pitchEst=%.1f", cyclePhase, setupTick,
                  estYaw, estPitch);
        const ImVec2 ds = ImGui::CalcTextSize(line);
        ImGui::GetForegroundDrawList()->AddText(
            ImVec2(io.DisplaySize.x * 0.5f - ds.x * 0.5f, io.DisplaySize.y - 32.0f),
            IM_COL32(255, 255, 255, 255), line);
    }
}

void Telly::DrawContent()
{
    if (closed)
    {
        isEnabled = false;
        closed = false;
        stopAutomation("window closed");
    }

    if (running)
        ImGuiStd::TextColoredShadow(ImVec4(0.1f, 1.0f, 0.1f, 1.0f), u8"搭路中");
    else if (activatePromptAt != 0)
        ImGuiStd::TextColoredShadow(activationReady() ? ImVec4(0.33f, 1.0f, 0.33f, 1.0f)
                                                      : ImVec4(1.0f, 0.33f, 0.33f, 1.0f),
                                    u8"激活中...");
    else
        ImGuiStd::TextShadow(u8"待激活");

    if (debugLog)
    {
        char buf[128];
        sprintf_s(buf, u8"相位 %d / setup %d", cyclePhase, setupTick);
        ImGuiStd::TextShadow(buf);
    }

    dirtyState.animating = false;
}

void Telly::HoverSetting()
{
}

// ---------------------------------------------------------------- 设置界面

void Telly::DrawSettings(const float& bigPadding, const float& centerX, const float& itemWidth)
{
    // ---- 原始设置（名字、默认值、范围与 Java 侧一致）----
    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox("Auto Swap", &autoSwap);
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"原版：手持方块少于 5 个时自动切到最多的那一格。\n"
                         u8"需要读背包数据（Java 侧能力），本 DLL 不读游戏内存，此项当前不生效。");

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox("Disable SafeWalk", &disableSafeWalk);
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"原版：运行期间关掉同名 SafeWalk 模块。\n"
                         u8"本 DLL 里没有 SafeWalk 模块可关，此项当前不生效。");

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox("Show Activation Hitbox", &showActivationHitbox);
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"原版：把激活用的方块面画成 3D 框。\n"
                         u8"需要读世界方块（Java 侧能力），本 DLL 不读游戏内存，此项当前不生效。");

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox("Print", &print);
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::Checkbox("Debug Log", &debugLog);
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"诊断行写 DebugView（OutputDebugString），并显示屏幕底部诊断行。");

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    float actTime = (float)activationTime;
    if (ImGui::SliderFloat("Activation Time", &actTime, 0.1f, 2.0f, "%.1f s"))
        activationTime = (double)actTime;
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX + bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    float snap = (float)snapBase;
    if (ImGui::SliderFloat("Snap Degrees", &snap, 42.0f, 48.0f, "%.1f"))
        snapBase = (double)snap;
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"原版：激活时把朝向吸附到 round((yaw-该值)/90)*90+该值 的网格，\n"
                         u8"默认 46.5（不是 45：45 是整度数，吸附后角度太“机器”，实测被 Intave 报 acting computer-like）。\n"
                         u8"DLL 侧拿不到绝对 yaw，此值只作为曲线基准角记录，不做绝对吸附。");

    // ---- DLL 侧新增（Java 侧不需要的宿主参数）----
    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    float sensValue = (float)sensitivity;
    if (ImGui::SliderFloat(u8"客户端灵敏度", &sensValue, 0.0f, 1.0f, "%.3f"))
        sensitivity = (double)sensValue;
    ImGui::SameLine();
    ImGuiStd::HelpMarker(u8"用于把曲线的角度换算成鼠标计数：\n"
                         u8"每计数 = (灵敏度*0.6+0.2)^3*8*0.15 度（0.5 时约 0.15°/计数）。\n"
                         u8"和游戏设置里的灵敏度保持一致，视角才会按曲线走。");

    ImGui::SetCursorPosX(bigPadding);
    ImGui::SetNextItemWidth(itemWidth);
    ImGui::SliderInt(u8"运行上限 秒", &maxRunSeconds, 5, 300);
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
    ImGuiStd::HelpMarker(u8"外置注入：点击和转头都交给外部 ACTGInjector 发出，不带注入标记。\n"
                         u8"搭路的视角驱动对时序敏感，建议用 hid-driver / hid-serial 后端。");

    DrawKeybindSettings(bigPadding, centerX, itemWidth);
    DrawAffixSettings(bigPadding, centerX, itemWidth);
    DrawSoundSettings(bigPadding, centerX, itemWidth);
    DrawWindowSettings(bigPadding, centerX, itemWidth);
}

// ---------------------------------------------------------------- 配置

void Telly::Load(const nlohmann::json& j)
{
    LoadItem(j);
    LoadWindow(j);
    LoadKeybind(j);
    LoadAffix(j);
    LoadSound(j);

    if (j.contains("autoSwap")) autoSwap = j["autoSwap"];
    if (j.contains("disableSafeWalk")) disableSafeWalk = j["disableSafeWalk"];
    if (j.contains("showActivationHitbox")) showActivationHitbox = j["showActivationHitbox"];
    if (j.contains("print")) print = j["print"];
    if (j.contains("debugLog")) debugLog = j["debugLog"];
    if (j.contains("activationTime")) activationTime = j["activationTime"];
    if (j.contains("snapBase")) snapBase = j["snapBase"];
    if (j.contains("inputMode")) inputMode = j["inputMode"];
    if (j.contains("sensitivity")) sensitivity = j["sensitivity"];
    if (j.contains("maxRunSeconds")) maxRunSeconds = j["maxRunSeconds"];
}

void Telly::Save(nlohmann::json& j) const
{
    SaveItem(j);
    SaveWindow(j);
    SaveKeybind(j);
    SaveAffix(j);
    SaveSound(j);

    j["autoSwap"] = autoSwap;
    j["disableSafeWalk"] = disableSafeWalk;
    j["showActivationHitbox"] = showActivationHitbox;
    j["print"] = print;
    j["debugLog"] = debugLog;
    j["activationTime"] = activationTime;
    j["snapBase"] = snapBase;
    j["inputMode"] = inputMode;
    j["sensitivity"] = sensitivity;
    j["maxRunSeconds"] = maxRunSeconds;
}