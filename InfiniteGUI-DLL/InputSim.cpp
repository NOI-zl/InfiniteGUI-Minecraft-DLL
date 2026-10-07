#pragma once

#include "InputSim.h"

#include <atomic>
#include <cstdio>

namespace InputSim
{
    namespace
    {
        std::atomic<HWND> g_gameWnd{ nullptr };
        std::atomic<int> g_mode{ (int)Mode::SendInput };

        // 512 个键位各自的上次注入时间（GetTickCount64 毫秒）
        // 必须显式零初始化：C++17 里 std::atomic 的默认构造不写值，
        // 留成垃圾值会让 InjectedWithin() 误判"刚注入过"，把真实点击吃掉。
        std::atomic<unsigned long long> g_lastInject[512] = {};

        inline void MarkInject(int vk)
        {
            if (vk > 0 && vk < 512) g_lastInject[vk].store(NowMs(), std::memory_order_relaxed);
        }

        bool EnsureExternalImpl();

        inline int MouseFlagDown(int vk)
        {
            switch (vk)
            {
            case VK_LBUTTON:  return MOUSEEVENTF_LEFTDOWN;
            case VK_RBUTTON:  return MOUSEEVENTF_RIGHTDOWN;
            case VK_MBUTTON:  return MOUSEEVENTF_MIDDLEDOWN;
            case VK_XBUTTON1: return MOUSEEVENTF_XDOWN;
            case VK_XBUTTON2: return MOUSEEVENTF_XDOWN;
            default:          return 0;
            }
        }

        inline int MouseFlagUp(int vk)
        {
            switch (vk)
            {
            case VK_LBUTTON:  return MOUSEEVENTF_LEFTUP;
            case VK_RBUTTON:  return MOUSEEVENTF_RIGHTUP;
            case VK_MBUTTON:  return MOUSEEVENTF_MIDDLEUP;
            case VK_XBUTTON1: return MOUSEEVENTF_XUP;
            case VK_XBUTTON2: return MOUSEEVENTF_XUP;
            default:          return 0;
            }
        }

        inline bool IsMouseVk(int vk)
        {
            return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON
                || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
        }

        // XBUTTON 要区分 1/2，其余不带
        inline DWORD XButtonData(int vk)
        {
            if (vk == VK_XBUTTON1) return XBUTTON1;
            if (vk == VK_XBUTTON2) return XBUTTON2;
            return 0;
        }

        void PostMouse(HWND hwnd, bool down, int vk)
        {
            if (!hwnd) return;
            POINT pt{ 0, 0 };
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            LPARAM lp = MAKELPARAM(pt.x, pt.y);

            UINT msg = 0;
            WPARAM wparam = 0;
            int  xbtn = 0;

            switch (vk)
            {
            case VK_LBUTTON:
                msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                wparam = down ? MK_LBUTTON : 0;
                break;
            case VK_RBUTTON:
                msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                wparam = down ? MK_RBUTTON : 0;
                break;
            case VK_MBUTTON:
                msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                wparam = down ? MK_MBUTTON : 0;
                break;
            case VK_XBUTTON1:
            case VK_XBUTTON2:
                msg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                xbtn = (vk == VK_XBUTTON1) ? XBUTTON1 : XBUTTON2;
                wparam = MAKEWPARAM(down ? MK_XBUTTON1 : 0, xbtn);
                break;
            default:
                return;
            }
            PostMessage(hwnd, msg, wparam, lp);
        }
    } // namespace

    // ---------------------------------------------------------------- 外置隐藏层
    namespace
    {
        AcInput::Shared* g_extShared = nullptr;
        HANDLE           g_extEvent = nullptr;
        HANDLE           g_extMap = nullptr;

        void ExtNames(char* mapName, size_t mapLen, char* evtName, size_t evtLen)
        {
            const unsigned long pid = GetCurrentProcessId();
            sprintf_s(mapName, mapLen, "Local\\ACTG_IN_%lu", pid);
            sprintf_s(evtName, evtLen, "Local\\ACTG_EV_%lu", pid);
        }

        void PushExternal(AcInput::Cmd cmd, unsigned vk, int dx, int dy, unsigned holdMs)
        {
            if (!EnsureExternalImpl()) return;

            AcInput::Shared* sh = g_extShared;
            const long head = sh->head;
            const long tail = sh->tail;
            if ((head - tail) >= (long)AcInput::kRingSize)
            {
                InterlockedIncrement(&sh->dropped);
                return;
            }

            AcInput::Slot& slot = sh->ring[head & (long)(AcInput::kRingSize - 1)];
            slot.cmd = (unsigned)cmd;
            slot.vk = vk;
            slot.dx = dx;
            slot.dy = dy;
            slot.holdMs = holdMs;
            slot.reserved = 0;
            MemoryBarrier();
            InterlockedIncrement(&sh->head);

            if (g_extEvent) SetEvent(g_extEvent);
        }
    }

    void SetGameWindow(HWND hwnd) { g_gameWnd.store(hwnd, std::memory_order_relaxed); }
    HWND GetGameWindow() { return g_gameWnd.load(std::memory_order_relaxed); }

    void SetMode(Mode mode) { g_mode.store((int)mode, std::memory_order_relaxed); }
    Mode GetMode() { return (Mode)g_mode.load(std::memory_order_relaxed); }

    unsigned long long NowMs() { return GetTickCount64(); }

    void MouseMoveRel(int dx, int dy)
    {
        if (dx == 0 && dy == 0) return;
        const Mode mode = GetMode();

        if (mode == Mode::External) { PushExternal(AcInput::Cmd::MouseMove, 0, dx, dy, 0); return; }

        if (mode == Mode::PostMessage)
        {
            HWND hwnd = GetGameWindow();
            if (!hwnd) return;
            POINT pt{ 0, 0 };
            GetCursorPos(&pt);                 // 游戏窗口内的相对位移交给 WM_MOUSEMOVE
            ScreenToClient(hwnd, &pt);
            PostMessage(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x + dx, pt.y + dy));
            return;
        }

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = dx;
        in.mi.dy = dy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        in.mi.dwExtraInfo = kInjectTag;
        if (mode == Mode::MouseEvent)
        {
            mouse_event(MOUSEEVENTF_MOVE, (DWORD)dx, (DWORD)dy, 0, kInjectTag);
        }
        else
        {
            SendInput(1, &in, sizeof(INPUT));
        }
    }

    void MouseDown(int vkButton)
    {
        const Mode mode = GetMode();
        MarkInject(vkButton);

        if (mode == Mode::External) { PushExternal(AcInput::Cmd::MouseDown, (unsigned)vkButton, 0, 0, 0); return; }
        if (mode == Mode::PostMessage) { PostMouse(GetGameWindow(), true, vkButton); return; }
        if (mode == Mode::MouseEvent)
        {
            mouse_event(MouseFlagDown(vkButton), 0, 0, XButtonData(vkButton), kInjectTag);
            return;
        }
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MouseFlagDown(vkButton);
        in.mi.mouseData = XButtonData(vkButton);
        in.mi.dwExtraInfo = kInjectTag;
        SendInput(1, &in, sizeof(INPUT));
    }

    void MouseUp(int vkButton)
    {
        const Mode mode = GetMode();
        MarkInject(vkButton);

        if (mode == Mode::External) { PushExternal(AcInput::Cmd::MouseUp, (unsigned)vkButton, 0, 0, 0); return; }
        if (mode == Mode::PostMessage) { PostMouse(GetGameWindow(), false, vkButton); return; }
        if (mode == Mode::MouseEvent)
        {
            mouse_event(MouseFlagUp(vkButton), 0, 0, XButtonData(vkButton), kInjectTag);
            return;
        }
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MouseFlagUp(vkButton);
        in.mi.mouseData = XButtonData(vkButton);
        in.mi.dwExtraInfo = kInjectTag;
        SendInput(1, &in, sizeof(INPUT));
    }

    void MouseClick(int vkButton, int holdMs)
    {
        MouseDown(vkButton);
        if (holdMs > 0) Sleep(holdMs);
        MouseUp(vkButton);
    }

    void KeyDown(int vk)
    {
        const Mode mode = GetMode();
        MarkInject(vk);

        if (mode == Mode::External) { PushExternal(AcInput::Cmd::KeyDown, (unsigned)vk, 0, 0, 0); return; }
        if (mode == Mode::PostMessage)
        {
            HWND hwnd = GetGameWindow();
            if (hwnd) PostMessage(hwnd, WM_KEYDOWN, (WPARAM)vk, 0);
            return;
        }
        if (mode == Mode::MouseEvent) { keybd_event((BYTE)vk, 0, 0, kInjectTag); return; }
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = (WORD)vk;
        in.ki.dwExtraInfo = kInjectTag;
        if (vk == VK_RCONTROL || vk == VK_RMENU) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &in, sizeof(INPUT));
    }

    void KeyUp(int vk)
    {
        const Mode mode = GetMode();
        MarkInject(vk);

        if (mode == Mode::External) { PushExternal(AcInput::Cmd::KeyUp, (unsigned)vk, 0, 0, 0); return; }
        if (mode == Mode::PostMessage)
        {
            HWND hwnd = GetGameWindow();
            if (hwnd) PostMessage(hwnd, WM_KEYUP, (WPARAM)vk, 0);
            return;
        }
        if (mode == Mode::MouseEvent) { keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, kInjectTag); return; }
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = (WORD)vk;
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        in.ki.dwExtraInfo = kInjectTag;
        if (vk == VK_RCONTROL || vk == VK_RMENU) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &in, sizeof(INPUT));
    }

    void KeyClick(int vk, int holdMs)
    {
        KeyDown(vk);
        if (holdMs > 0) Sleep(holdMs);
        KeyUp(vk);
    }

    unsigned long long LastInjectMs(int vk)
    {
        if (vk <= 0 || vk >= 512) return 0;
        return g_lastInject[vk].load(std::memory_order_relaxed);
    }

    bool InjectedWithin(int vk, unsigned long long windowMs)
    {
        const unsigned long long last = LastInjectMs(vk);
        if (last == 0) return false;
        const unsigned long long now = NowMs();
        return now >= last && (now - last) <= windowMs;
    }

    // ---------------------------------------------------------------- 外置隐藏层实现
    // 注意：EnsureExternalImpl 的声明在文件上方的匿名命名空间里，
    // 定义必须也在匿名命名空间内，否则会变成两个不同的函数（调用歧义）。
    namespace
    {
        bool EnsureExternalImpl()
        {
        if (g_extShared) return true;
        if (g_extMap) return false;   // 建立过但失败，不再反复重试

        char mapName[64];
        char evtName[64];
        ExtNames(mapName, sizeof(mapName), evtName, sizeof(evtName));

        g_extMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapName);
        if (!g_extMap)
        {
            g_extMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          (DWORD)sizeof(AcInput::Shared), mapName);
        }
        if (!g_extMap) return false;

        void* view = MapViewOfFile(g_extMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AcInput::Shared));
        if (!view)
        {
            CloseHandle(g_extMap);
            g_extMap = nullptr;
            return false;
        }

        g_extShared = (AcInput::Shared*)view;
        if (g_extShared->magic != AcInput::kMagic)
        {
            g_extShared->magic = AcInput::kMagic;
            g_extShared->version = AcInput::kVersion;
            g_extShared->head = 0;
            g_extShared->tail = 0;
            g_extShared->aliveMs = 0;
        }

        g_extEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, evtName);
        if (!g_extEvent) g_extEvent = CreateEventA(nullptr, FALSE, FALSE, evtName);

        return true;
        }
    } // namespace

    bool ExternalEnsure() { return EnsureExternalImpl(); }

    bool ExternalConnected()
    {
        if (!EnsureExternalImpl()) return false;
        return g_extShared->aliveMs != 0;
    }

    bool ExternalAlive(unsigned long long freshMs)
    {
        if (!EnsureExternalImpl()) return false;
        const unsigned long long alive = (unsigned long long)g_extShared->aliveMs;
        if (alive == 0) return false;
        const unsigned long long now = NowMs();
        return now >= alive && (now - alive) <= freshMs;
    }

    const char* ExternalBackendName()
    {
        if (!EnsureExternalImpl()) return "未连接";
        switch ((AcInput::Backend)g_extShared->backend)
        {
        case AcInput::Backend::SendInput:    return "SendInput";
        case AcInput::Backend::MouseEvent:   return "MouseEvent";
        case AcInput::Backend::PostMessage:  return "PostMessage";
        case AcInput::Backend::HidDriver:    return "内核过滤驱动";
        case AcInput::Backend::VulnDriver:   return "内核端口(BYOVD)";
        case AcInput::Backend::HidSerial:    return "外置硬件HID";
        case AcInput::Backend::UsermodeHide: return "SendInput+游戏内隐藏";
        default:                             return "未知";
        }
    }

    // ---------------------------------------------------------------- 外置 AC 引擎

    void UseHold(bool pressed)
    {
        if (GetMode() == Mode::External)
        {
            PushExternal(AcInput::Cmd::UseHold, VK_RBUTTON, 0, 0, pressed ? 1u : 0u);
            MarkInject(VK_RBUTTON);
            return;
        }
        // 本地兜底：就是按住/抬起右键（与原版语义一致）
        if (pressed) MouseDown(VK_RBUTTON);
        else         MouseUp(VK_RBUTTON);
    }

    void PlaceClick(int holdMs)
    {
        if (holdMs <= 0) holdMs = 60;

        if (GetMode() == Mode::External)
        {
            PushExternal(AcInput::Cmd::PlaceClick, VK_RBUTTON, 0, 0, (unsigned)holdMs);
            MarkInject(VK_RBUTTON);
            return;
        }
        MouseClick(VK_RBUTTON, holdMs);
    }

    void SetAcProfile(const AcInput::AcProfile& profile)
    {
        if (!EnsureExternalImpl()) return;

        AcInput::AcProfile& dst = g_extShared->ac;
        const bool same =
            dst.engineOn == profile.engineOn &&
            dst.mode == profile.mode &&
            dst.useAsClicks == profile.useAsClicks &&
            dst.leftEnabled == profile.leftEnabled &&
            dst.leftCps == profile.leftCps &&
            dst.rightEnabled == profile.rightEnabled &&
            dst.rightCps == profile.rightCps &&
            dst.jitterPct == profile.jitterPct &&
            dst.holdMin == profile.holdMin &&
            dst.holdMax == profile.holdMax &&
            dst.extra == profile.extra &&
            dst.longMs == profile.longMs;
        if (same) return;

        dst.engineOn = profile.engineOn;
        dst.mode = profile.mode;
        dst.useAsClicks = profile.useAsClicks;
        dst.leftEnabled = profile.leftEnabled;
        dst.leftCps = profile.leftCps;
        dst.rightEnabled = profile.rightEnabled;
        dst.rightCps = profile.rightCps;
        dst.jitterPct = profile.jitterPct;
        dst.holdMin = profile.holdMin;
        dst.holdMax = profile.holdMax;
        dst.extra = profile.extra;
        dst.longMs = profile.longMs;
        MemoryBarrier();
        InterlockedIncrement((volatile long*)&dst.seq);
    }

    long ExternalClicksDone()
    {
        if (!EnsureExternalImpl()) return 0;
        return g_extShared->clicksDone;
    }

    bool ExternalUseHeld()
    {
        if (!EnsureExternalImpl()) return false;
        return g_extShared->useHeld != 0;
    }
}