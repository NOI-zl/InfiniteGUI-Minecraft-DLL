#pragma once

#include <Windows.h>

#include "AcInputLink.h"

// ============================================================================
// InputSim —— 统一输入注入层
//
// 为什么不用上游的 KeyState：
//   KeyState::SetKeyDown/Up/Click 的 SC_SEND_INPUT 分支里，判断了
//   `key == Click::Left || key == Click::Right`，但两个分支发的都是
//   MOUSEEVENTF_LEFTDOWN / MOUSEEVENTF_LEFTUP —— **右键注入实际发出的是左键**；
//   SC_POST_MESSAGE 分支同理，一律 PostMessage(WM_LBUTTONDOWN/UP)。
//   AC / Telly / 蹲起塔 都依赖可靠的右键（放置方块），所以这里自己实现一份
//   左右键区分正确的注入，并且不改动上游任何文件。
//
// 另外提供「相对鼠标移动」——驱动视角用（Telly 的 yaw/pitch 曲线靠它落地）。
// 所有注入事件都打上 dwExtraInfo 标记，便于识别"这是自己发的"。
// ============================================================================

namespace InputSim
{
    enum class Mode
    {
        SendInput = 0,   // SendInput（默认，兼容性最好）
        MouseEvent = 1,  // mouse_event / keybd_event（老客户端更稳）
        PostMessage = 2, // 直接往游戏窗口 PostMessage（被反作弊忽略，但部分客户端不响应）
        External = 3     // 交给外置注入器：按键/转头不带 INJECTED 标记（隐藏/绕过）
    };

    constexpr ULONG_PTR kInjectTag = 0x41435447; // 'ACTG'

    // 游戏窗口句柄（PostMessage 模式需要）。可传 nullptr，之后用 SetGameWindow 补。
    void SetGameWindow(HWND hwnd);
    HWND GetGameWindow();

    void SetMode(Mode mode);
    Mode GetMode();

    // 相对鼠标移动（视角）。返回值：实际发出的计数（PostMessage 模式下为 0）。
    void MouseMoveRel(int dx, int dy);

    // 鼠标键：VK_LBUTTON / VK_RBUTTON / VK_MBUTTON / VK_XBUTTON1 / VK_XBUTTON2
    void MouseDown(int vkButton);
    void MouseUp(int vkButton);
    void MouseClick(int vkButton, int holdMs);

    // 键盘
    void KeyDown(int vk);
    void KeyUp(int vk);
    void KeyClick(int vk, int holdMs);

    // 上一次对该键/键位注入的时间（GetTickCount64 毫秒）。
    // AC 用它来忽略"自己发出去的点击"被 GetAsyncKeyState 读回来造成的自激。
    unsigned long long LastInjectMs(int vk);
    bool InjectedWithin(int vk, unsigned long long windowMs);

    unsigned long long NowMs();

    // ---- 外置隐藏层（Mode::External）----
    // 与「外置注入器」建立共享内存通道（先起哪边都行）。
    bool ExternalEnsure();
    bool ExternalConnected();
    // 注入器当前实际使用的后端名（SendInput / HidDriver / VulnDriver / HidSerial / UsermodeHide）
    const char* ExternalBackendName();
    // 注入器心跳是否新鲜（用于判断外置程序是否还活着）
    bool ExternalAlive(unsigned long long freshMs = 2000);

    // ---- 外置 AC 引擎（点击调度搬到外置程序里执行）----
    // Telly 的「放置状态」：外置端可直接按住右键，也可按 useAsClicks 转成 CPS 点击。
    void UseHold(bool pressed);
    // 一次性右键点击（蹲起塔 / 需要单次放置时）
    void PlaceClick(int holdMs = 60);
    // 推送 AC 配置给外置引擎（内部做变更检测，可以每帧调）
    void SetAcProfile(const AcInput::AcProfile& profile);
    // 外置 AC 引擎已发出的点击数 / use 是否处于按住
    long ExternalClicksDone();
    bool ExternalUseHeld();
}