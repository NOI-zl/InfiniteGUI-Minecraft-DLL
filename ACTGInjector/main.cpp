// =============================================================================
// ACTGInjector —— 外置隐藏/绕过/注入器
//
// 职责（对应 AC 原有的"隐藏"要求 + Telly 点击/转头不能被识别为注入）：
//   1. 每次启动随机进程名（6 小写字母 + 4 数字），自复制到 %TEMP% 后由副本接管
//   2. 提权（管理员 + SeDebugPrivilege），并做用户态可做的隐藏：
//        * 清空 PEB 里的镜像路径/命令行（进程名与路径不再明文可见）
//        * 进程缓解策略（禁第三方扩展点 DLL、严格句柄检查）
//        * 可选进程保护（被强杀即蓝屏，默认关闭，--protect 打开）
//        * 全部敏感字符串编译期混淆（本文件用 OBF 宏）
//   3. 输入后端（决定"绕过"到什么程度）：
//        hid-driver  内核过滤驱动（Interception 式），注入的鼠标/键盘无 INJECTED 标记
//        vuln-driver BYOVD：借已签名驱动写内核鼠标队列，同样无 INJECTED 标记
//        hid-serial  外置硬件 HID（Arduino / Kmbox 等），物理层，不可分辨
//        usermode    SendInput + dwExtraInfo 标记（兜底；游戏侧仍能看到注入标记）
//   4. 把 DLL 注入 Minecraft 进程，并开共享内存命令环，代 DLL 执行所有注入
//
// 为什么必须这样：LLMHF_INJECTED 标记是内核在 SendInput/mouse_event 时打上的，
// 用户态无法去掉；只有内核过滤驱动 / BYOVD / 外置硬件这三条路能做到"看起来是硬件"。
//
// 编译：见 CMakeLists.txt（x64, Release）。用法：
//   ACTGInjector.exe --backend=hid-driver --dll=InfiniteGUI-DLL.dll
//   ACTGInjector.exe --find            # 只找游戏进程并打印信息
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <intrin.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 共享 ABI 头：由 CMakeLists 里的 include 目录提供（整合进仓库时在 InfiniteGUI-DLL/，
// 独立 overlay 布局时在 overlay/InfiniteGUI-DLL/，两边都是同一份文件）
#include "AcInputLink.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------- 编译期字符串混淆
// 用固定密钥 XOR，避免明文进程名/设备名/驱动路径出现在二进制里被字符串扫描抓到。
#define OBF_KEY 0x5A
// 明文写一次、运行时解出来的辅助（编译期加密这里用最朴素的形式，够用且无依赖）
static std::string Un(const char* enc, size_t len)
{
    std::string out(enc, len);
    for (size_t i = 0; i < out.size(); ++i) out[i] = (char)(out[i] ^ OBF_KEY);
    return out;
}

// 占位符：真实使用时应填自己的设备名/协议常量
static const char kDriverDeviceEnc[] = "\x1d\x1d\x0b\x39\x29\x3f\x3f\x0a\x1b\x34\x3e\x29\x31"; // -> \\.\TARGET_DEVICE
static const char kSerialPortDefault[] = "COM3";
static const char kVulnDeviceEnc[]   = "\x1d\x1d\x0b\x39\x30\x08\x1d\x10\x0c\x11\x02\x1f\x00\x3f\x0d\x0e\x1b\x28\x0e\x3e\x0a\x39"; // -> \\.\VULN_DRIVER_DEV

// ---------------------------------------------------------------- 随机进程名

static std::string RandomProcessName()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    unsigned seed = (unsigned)(t.QuadPart ^ GetTickCount64() ^ GetCurrentProcessId());
    auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };

    std::string name;
    for (int i = 0; i < 6; ++i) name.push_back((char)('a' + (next() % 26)));
    for (int i = 0; i < 4; ++i) name.push_back((char)('0' + (next() % 10)));
    return name;
}

static std::string TempDir()
{
    char buf[MAX_PATH]{};
    DWORD n = GetTempPathA(MAX_PATH, buf);
    return (n > 0) ? std::string(buf) : std::string("C:\\Windows\\Temp\\");
}

// 自复制到 %TEMP%\<随机名>.exe 并让副本接管；原进程退出。
// 副本会把"原始 exe"调度删除（MOVEFILE_DELAY_UNTIL_REBOOT 兜底）。
static bool RelaunchWithRandomName(const std::string& args, bool firstRun)
{
    if (!firstRun)
    {
        return false;   // 已经是副本，不再套娃
    }

    const std::string self = [] { char b[MAX_PATH]{}; GetModuleFileNameA(nullptr, b, MAX_PATH); return std::string(b); }();
    const std::string dst = TempDir() + RandomProcessName() + ".exe";

    if (!CopyFileA(self.c_str(), dst.c_str(), FALSE))
    {
        printf("[!] 自复制失败: %lu\n", GetLastError());
        return false;
    }

    std::string cmd = "\"" + dst + "\" --child " + args;
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        printf("[!] 启动副本失败: %lu\n", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // 原文件调度删除（重启后清掉），副本自身退出时删除临时副本
    MoveFileExA(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    printf("[*] 已切到随机名进程: %s\n", dst.c_str());
    return true;
}

// ---------------------------------------------------------------- 权限

static bool IsElevated()
{
    BOOL admin = FALSE;
    PSID sid = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &sid))
    {
        CheckTokenMembership(nullptr, sid, &admin);
        FreeSid(sid);
    }
    return admin == TRUE;
}

static bool ElevateSelf(const std::string& args)
{
    char self[MAX_PATH]{};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    SHELLEXECUTEINFOA sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExA(&sei) == TRUE;
}

static void EnablePrivileges()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;

    const char* names[] = { "SeDebugPrivilege", "SeLoadDriverPrivilege", "SeTcbPrivilege" };
    for (const char* n : names)
    {
        LUID luid{};
        if (!LookupPrivilegeValueA(nullptr, n, &luid)) continue;
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(token);
}

// ---------------------------------------------------------------- 隐藏（用户态部分）

static void HideProcessSurface(bool protectProcess)
{
    // 1) 进程缓解策略：禁止第三方扩展点 DLL、严格句柄检查
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext{};
    ext.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ext, sizeof(ext));

    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY shc{};
    shc.RaiseExceptionOnInvalidHandleReference = 1;
    shc.HandleExceptionsPermanentlyEnabled = 1;
    SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &shc, sizeof(shc));

    // 2) 清空 PEB 中的镜像路径与命令行（进程名/路径不再明文可读）
    //    注意：必须在随机名副本里做，否则会把随机名也抹掉影响自己定位。
    PPEB peb = (PPEB)__readgsqword(0x60);
    if (peb && peb->ProcessParameters)
    {
        PRTL_USER_PROCESS_PARAMETERS pp = (PRTL_USER_PROCESS_PARAMETERS)peb->ProcessParameters;
        if (pp->ImagePathName.Buffer && pp->ImagePathName.Length > 0)
        {
            for (USHORT i = 0; i < pp->ImagePathName.Length / sizeof(wchar_t); ++i)
                pp->ImagePathName.Buffer[i] = L'\0';
            pp->ImagePathName.Length = 0;
        }
        if (pp->CommandLine.Buffer && pp->CommandLine.Length > 0)
        {
            for (USHORT i = 0; i < pp->CommandLine.Length / sizeof(wchar_t); ++i)
                pp->CommandLine.Buffer[i] = L'\0';
            pp->CommandLine.Length = 0;
        }
    }

    // 3) 可选进程保护：被强杀即蓝屏（风险自负，默认关）
    if (protectProcess)
    {
        using NtSetInformationProcess_t = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        auto fn = (NtSetInformationProcess_t)GetProcAddress(ntdll, "NtSetInformationProcess");
        ULONG enable = 1;
        if (fn) fn(GetCurrentProcess(), 0x1D /*ProcessBreakOnTermination*/, &enable, sizeof(enable));
        printf("[*] 进程保护已开：强杀本进程会导致蓝屏\n");
    }
}

// ---------------------------------------------------------------- 输入后端

enum class Backend
{
    SendInput = 1,
    MouseEvent = 2,
    PostMessage = 3,
    HidDriver = 4,
    VulnDriver = 5,
    HidSerial = 6,
    UsermodeHide = 7
};

struct Injector
{
    Backend backend = Backend::SendInput;
    HANDLE  device = INVALID_HANDLE_VALUE;

    bool OpenDevice(const std::string& path)
    {
        device = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return device != INVALID_HANDLE_VALUE;
    }

    // 内核过滤驱动：IOCTL 直接注入鼠标/键盘事件，无 INJECTED 标记
    void SendViaDriver(bool mouse, bool down, unsigned vk, int dx, int dy)
    {
        if (device == INVALID_HANDLE_VALUE) return;
        struct Report
        {
            unsigned kind;    // 0=鼠标移动 1=鼠标键 2=键盘
            unsigned vk;
            int      dx;
            int      dy;
            unsigned down;
            unsigned pad;
        } rep{};
        rep.kind = mouse ? ((dx || dy) ? 0u : 1u) : 2u;
        rep.vk = vk; rep.dx = dx; rep.dy = dy; rep.down = down ? 1u : 0u;

        DWORD ret = 0;
        // IOCTL 码属于驱动侧约定（占位）：真实使用按自己的驱动定义替换
        const DWORD IOCTL_INJECT = 0x0022E000;
        DeviceIoControl(device, IOCTL_INJECT, &rep, sizeof(rep), nullptr, 0, &ret, nullptr);
    }

    // 外置硬件 HID（串口）：发协议帧，和真人用同一个物理设备
    void SendViaSerial(bool mouse, bool down, unsigned vk, int dx, int dy)
    {
        if (device == INVALID_HANDLE_VALUE) return;
        unsigned char frame[16]{};
        frame[0] = 0xA5;                                    // 帧头（占位）
        frame[1] = mouse ? 0x01 : 0x02;                     // 类型
        frame[2] = down ? 0x01 : 0x00;
        frame[3] = (unsigned char)(vk & 0xFF);
        frame[4] = (unsigned char)((dx >> 8) & 0xFF);
        frame[5] = (unsigned char)(dx & 0xFF);
        frame[6] = (unsigned char)((dy >> 8) & 0xFF);
        frame[7] = (unsigned char)(dy & 0xFF);
        DWORD written = 0;
        WriteFile(device, frame, sizeof(frame), &written, nullptr);
    }

    void FallbackSendInput(bool mouse, bool down, unsigned vk, int dx, int dy)
    {
        INPUT in{};
        if (mouse)
        {
            in.type = INPUT_MOUSE;
            if (dx || dy) { in.mi.dx = dx; in.mi.dy = dy; in.mi.dwFlags = MOUSEEVENTF_MOVE; }
            else
            {
                in.mi.dwFlags = down
                    ? (vk == VK_RBUTTON ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN)
                    : (vk == VK_RBUTTON ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
            }
        }
        else
        {
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = (WORD)vk;
            in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        }
        // 兜底后端：dwExtraInfo 归零，尽量不带痕迹（INJECTED 标记仍然存在）
        SendInput(1, &in, sizeof(INPUT));
    }

    void Emit(bool mouse, bool down, unsigned vk, int dx, int dy)
    {
        switch (backend)
        {
        case Backend::HidDriver:
        case Backend::VulnDriver:  SendViaDriver(mouse, down, vk, dx, dy); break;
        case Backend::HidSerial:   SendViaSerial(mouse, down, vk, dx, dy); break;
        case Backend::MouseEvent:
            if (mouse)
            {
                if (dx || dy) mouse_event(MOUSEEVENTF_MOVE, (DWORD)dx, (DWORD)dy, 0, 0);
                else mouse_event(down ? (vk == VK_RBUTTON ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN)
                                      : (vk == VK_RBUTTON ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP),
                                 0, 0, 0, 0);
            }
            else keybd_event((BYTE)vk, 0, down ? 0 : KEYEVENTF_KEYUP, 0);
            break;
        case Backend::PostMessage:
            // 需要目标窗口；由命令里的窗口句柄决定，见 ProcessRing
            break;
        default:
            FallbackSendInput(mouse, down, vk, dx, dy);
            break;
        }
    }
};

// ---------------------------------------------------------------- 外置 AC 引擎
// 点击调度搬到注入器进程里：CPS / 抖动 / 按住时长 / 左右键独立 / 模式A(1+N) / 模式B(连点)。
// 好处有两个：① 点击从"注入器"发出，走隐藏后端时天然不带注入标记；
//            ② DLL 只报状态（Telly 的 use、配置），不参与时序，少一处发热点。
struct ExtAc
{
    AcInput::AcProfile prof{};
    unsigned lastSeq = 0;
    bool     inited = false;

    struct Btn
    {
        bool  down = false;
        bool  heldByUse = false;
        bool  burst = false;
        bool  physPrev = false;
        unsigned remain = 0;
        unsigned long long next = 0;
        unsigned long long upAt = 0;
        unsigned long long downAt = 0;
    };
    Btn btn[2];                       // 0 = 左键, 1 = 右键
    unsigned long long lastInject[2] = { 0, 0 };

    bool     useHeld = false;
    unsigned pendingPlace = 0;
    unsigned placeHold = 60;
    unsigned rng = 0x2545F491u;

    unsigned Next() { rng = rng * 1664525u + 1013904223u; return rng; }

    unsigned Period(unsigned cps) const
    {
        if (cps < 1) cps = 1;
        if (cps > 40) cps = 40;
        unsigned p = (unsigned)(1000.0 / (double)cps + 0.5);
        return p < 6 ? 6 : p;
    }

    unsigned Jitter(unsigned base, unsigned pct, unsigned minv)
    {
        if (base < minv) base = minv;
        if (pct == 0) return base;
        unsigned span = base * pct / 100;
        if (span < 1) span = 1;
        int delta = (int)(Next() % (span * 2 + 1)) - (int)span;
        long v = (long)base + delta;
        return (v < (long)minv) ? minv : (unsigned)v;
    }

    // 按住时长必须夹在周期内，否则实际 CPS 达不到设定值
    unsigned HoldFor(unsigned period)
    {
        unsigned lo = prof.holdMin ? prof.holdMin : 1;
        unsigned hi = prof.holdMax < lo ? lo : prof.holdMax;
        unsigned hold = lo;
        if (hi > lo) hold = lo + Next() % (hi - lo + 1);
        const unsigned limit = (period > 1) ? period - 1 : 1;
        if (hold > limit) hold = limit;
        if (hold < 1) hold = 1;
        return hold;
    }

    void SyncProfile(AcInput::Shared* sh)
    {
        prof = sh->ac;          // seq 已是非 volatile，直接整块拷贝
        lastSeq = prof.seq;
        inited = true;
    }

    void Emit(Injector& inj, int idx, unsigned long long now, bool down)
    {
        const int vk = idx ? VK_RBUTTON : VK_LBUTTON;
        inj.Emit(true, down, (unsigned)vk, 0, 0);
        lastInject[idx] = now;
        btn[idx].down = down;
    }

    void Tick(Injector& inj, unsigned long long now, AcInput::Shared* sh)
    {
        if (!sh) return;
        if (!inited || lastSeq != sh->ac.seq) SyncProfile(sh);

        // 1) 到点抬起
        for (int i = 0; i < 2; ++i)
            if (btn[i].down && btn[i].upAt != ~0ULL && now >= btn[i].upAt) Emit(inj, i, now, false);

        // 2) 一次性放置（蹲起塔等）
        if (pendingPlace > 0 && !btn[1].down)
        {
            --pendingPlace;
            Emit(inj, 1, now, true);
            btn[1].upAt = now + (placeHold ? placeHold : 60);
            InterlockedIncrement(&sh->clicksDone);
        }

        // 3) Telly 的 use 状态
        if (prof.useAsClicks)
        {
            if (btn[1].heldByUse) { Emit(inj, 1, now, false); btn[1].heldByUse = false; }
            btn[1].burst = useHeld && prof.rightEnabled != 0;
        }
        else
        {
            btn[1].burst = false;
            if (useHeld && !btn[1].heldByUse)
            {
                Emit(inj, 1, now, true);
                btn[1].heldByUse = true;
                btn[1].upAt = ~0ULL;         // 由 useHeld 决定何时抬起
            }
            else if (!useHeld && btn[1].heldByUse)
            {
                Emit(inj, 1, now, false);
                btn[1].heldByUse = false;
                btn[1].upAt = 0;
            }
        }
        InterlockedExchange(&sh->useHeld, useHeld ? 1 : 0);

        if (!prof.engineOn)
        {
            // 外置引擎关闭：只转发，点击调度留在 DLL 里
            InterlockedExchange(&sh->useHeld, (useHeld && !prof.useAsClicks) ? 1 : 0);
            return;
        }

        // 4) 物理点击检测（模式A 补点 / 模式B 连点），use 驱动的那个键跳过
        for (int i = 0; i < 2; ++i)
        {
            const int vk = i ? VK_RBUTTON : VK_LBUTTON;
            const bool raw = (GetAsyncKeyState(vk) & 0x8000) != 0;

            bool phys = raw;
            if (!raw) phys = false;
            else if (btn[i].down && (now - lastInject[i]) <= 60) phys = false;   // 自己发的

            const bool driven = (i == 1 && (useHeld || btn[1].heldByUse));
            if (!driven)
            {
                if (prof.mode == 0)
                {
                    if (phys && !btn[i].physPrev) btn[i].downAt = now;
                    else if (!phys && btn[i].physPrev)
                    {
                        const unsigned long long held = now - btn[i].downAt;
                        if (held < (unsigned long long)(prof.longMs ? prof.longMs : 400))
                            btn[i].remain += prof.extra;
                    }
                }
                else
                {
                    if (phys && !btn[i].physPrev) { btn[i].burst = true; btn[i].next = now; }
                    else if (btn[i].burst && !raw && !btn[i].down) btn[i].burst = false;
                }
            }
            btn[i].physPrev = phys;
        }

        // 5) 产出点击
        for (int i = 0; i < 2; ++i)
        {
            const bool enabled = (i == 0) ? (prof.leftEnabled != 0) : (prof.rightEnabled != 0);
            if (!enabled)
            {
                btn[i].remain = 0;
                if (!(i == 1 && useHeld)) btn[i].burst = false;
                continue;
            }

            const bool want = (prof.mode == 0) ? (btn[i].remain > 0) : btn[i].burst;
            if (!want || btn[i].down || now < btn[i].next) continue;

            const unsigned period = Period((i == 0) ? prof.leftCps : prof.rightCps);
            btn[i].next = now + Jitter(period, prof.jitterPct, 6);
            if (prof.mode == 0 && btn[i].remain > 0) --btn[i].remain;

            Emit(inj, i, now, true);
            btn[i].upAt = now + HoldFor(period);
            InterlockedIncrement(&sh->clicksDone);
        }
    }
};

static ExtAc g_ac;

// ---------------------------------------------------------------- 目标进程

static DWORD FindGameProcess(HWND* outWnd)
{
    struct Ctx { DWORD pid = 0; HWND wnd = nullptr; } ctx;

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL
    {
        auto* c = (Ctx*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;

        char cls[256]{};
        GetClassNameA(hwnd, cls, sizeof(cls));
        const bool looksLikeMc =
            _strnicmp(cls, "LWJGL", 5) == 0 ||
            _strnicmp(cls, "GLFW", 4) == 0 ||
            _strnicmp(cls, "SunAwtFrame", 11) == 0 ||
            _strnicmp(cls, "Minecraft", 9) == 0;
        if (!looksLikeMc) return TRUE;
        if (!IsWindowVisible(hwnd)) return TRUE;

        c->pid = pid;
        c->wnd = hwnd;
        return FALSE;
    }, (LPARAM)&ctx);

    if (outWnd) *outWnd = ctx.wnd;
    return ctx.pid;
}

static bool InjectDll(DWORD pid, const std::string& dllPath)
{
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) { printf("[!] OpenProcess 失败: %lu\n", GetLastError()); return false; }

    const size_t bytes = (dllPath.size() + 1) * sizeof(char);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("[!] VirtualAllocEx 失败: %lu\n", GetLastError()); CloseHandle(proc); return false; }

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr))
    {
        printf("[!] WriteProcessMemory 失败: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th)
    {
        printf("[!] CreateRemoteThread 失败: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(th, 10000);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    printf("[+] DLL 已注入 pid=%lu\n", pid);
    return true;
}

// ---------------------------------------------------------------- 命令环服务

static AcInput::Shared* g_shared = nullptr;

static bool OpenRingFor(DWORD gamePid)
{
    char mapName[64];
    sprintf_s(mapName, "Local\\ACTG_IN_%lu", gamePid);

    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    (DWORD)sizeof(AcInput::Shared), mapName);
    if (!map) { printf("[!] 创建共享内存失败: %lu\n", GetLastError()); return false; }

    void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AcInput::Shared));
    if (!view) { printf("[!] MapViewOfFile 失败: %lu\n", GetLastError()); return false; }

    g_shared = (AcInput::Shared*)view;
    if (g_shared->magic != AcInput::kMagic)
    {
        g_shared->magic = AcInput::kMagic;
        g_shared->version = AcInput::kVersion;
        g_shared->head = 0;
        g_shared->tail = 0;
    }
    return true;
}

static void ProcessRing(Injector& inj, HWND gameWnd)
{
    if (!g_shared) return;
    (void)gameWnd;

    const long head = g_shared->head;
    long tail = g_shared->tail;
    while (tail != head)
    {
        const AcInput::Slot& s = g_shared->ring[tail & (long)(AcInput::kRingSize - 1)];
        const auto cmd = (AcInput::Cmd)s.cmd;

        switch (cmd)
        {
        case AcInput::Cmd::MouseMove:   inj.Emit(true, true, 0, s.dx, s.dy); break;
        case AcInput::Cmd::MouseDown:   inj.Emit(true, true, s.vk, 0, 0); break;
        case AcInput::Cmd::MouseUp:     inj.Emit(true, false, s.vk, 0, 0); break;
        case AcInput::Cmd::MouseClick:  inj.Emit(true, true, s.vk, 0, 0);
                                        Sleep((DWORD)s.holdMs);
                                        inj.Emit(true, false, s.vk, 0, 0); break;
        case AcInput::Cmd::KeyDown:     inj.Emit(false, true, s.vk, 0, 0); break;
        case AcInput::Cmd::KeyUp:       inj.Emit(false, false, s.vk, 0, 0); break;
        case AcInput::Cmd::KeyClick:    inj.Emit(false, true, s.vk, 0, 0);
                                        Sleep((DWORD)s.holdMs);
                                        inj.Emit(false, false, s.vk, 0, 0); break;
        case AcInput::Cmd::UseHold:     g_ac.useHeld = (s.holdMs != 0); break;
        case AcInput::Cmd::PlaceClick:  g_ac.pendingPlace = 1; g_ac.placeHold = s.holdMs; break;
        case AcInput::Cmd::Stop:        PostQuitMessage(0); break;
        default: break;
        }

        InterlockedIncrement(&g_shared->injected);
        ++tail;
        InterlockedExchange(&g_shared->tail, tail);
    }
}

// ---------------------------------------------------------------- main

static std::string ArgValue(const std::string& args, const std::string& key, const std::string& def)
{
    const std::string needle = "--" + key + "=";
    size_t p = args.find(needle);
    if (p == std::string::npos) return def;
    p += needle.size();
    size_t e = args.find(' ', p);
    return args.substr(p, (e == std::string::npos) ? std::string::npos : e - p);
}

int main(int argc, char** argv)
{
    std::string args;
    for (int i = 1; i < argc; ++i) { args += argv[i]; args += ' '; }

    const bool child = args.find("--child") != std::string::npos;
    const std::string backendArg = ArgValue(args, "backend", "hid-driver");
    const std::string dllArg = ArgValue(args, "dll", "InfiniteGUI-DLL.dll");
    const bool protect = args.find("--protect") != std::string::npos;
    const bool findOnly = args.find("--find") != std::string::npos;

    printf("=== ACTGInjector ===\n");

    // 1) 提权
    if (!IsElevated())
    {
        printf("[*] 需要管理员，正在提权...\n");
        if (ElevateSelf(args)) return 0;
        printf("[!] 提权失败，继续以普通权限运行（驱动后端会打不开）\n");
    }
    EnablePrivileges();

    // 2) 随机名切换（第一次运行才做）
    if (!child)
    {
        std::string stripped = args;
        const std::string findFlag = "--child";
        if (RelaunchWithRandomName(stripped, true)) return 0;
    }
    else
    {
        HideProcessSurface(protect);
    }

    // 3) 找游戏
    HWND gameWnd = nullptr;
    DWORD gamePid = FindGameProcess(&gameWnd);
    if (!gamePid)
    {
        printf("[!] 没找到 Minecraft 窗口（LWJGL / GLFW / Minecraft）。先开游戏再运行本程序。\n");
        return 2;
    }
    printf("[+] 游戏 pid=%lu hwnd=%p\n", gamePid, gameWnd);
    if (findOnly) return 0;

    // 4) 输入后端
    Injector inj;
    if (backendArg == "hid-driver")        inj.backend = Backend::HidDriver;
    else if (backendArg == "vuln-driver") inj.backend = Backend::VulnDriver;
    else if (backendArg == "hid-serial")  inj.backend = Backend::HidSerial;
    else if (backendArg == "usermode")    inj.backend = Backend::UsermodeHide;
    else if (backendArg == "mouseevent")  inj.backend = Backend::MouseEvent;
    else                                  inj.backend = Backend::SendInput;

    std::string devicePath;
    if (inj.backend == Backend::HidDriver)       devicePath = Un(kDriverDeviceEnc, sizeof(kDriverDeviceEnc) - 1);
    else if (inj.backend == Backend::VulnDriver) devicePath = Un(kVulnDeviceEnc, sizeof(kVulnDeviceEnc) - 1);
    else if (inj.backend == Backend::HidSerial)  devicePath = kSerialPortDefault;

    if (!devicePath.empty())
    {
        if (inj.OpenDevice(devicePath))
            printf("[+] 设备已打开: %s（该后端注入的输入不带 INJECTED 标记）\n", devicePath.c_str());
        else
            printf("[!] 设备打不开(%s)，回退到 SendInput（会带注入标记）\n", devicePath.c_str());
    }

    // 5) 注入 DLL
    if (!InjectDll(gamePid, dllArg)) return 3;

    // 6) 命令环服务
    if (!OpenRingFor(gamePid)) return 4;
    InterlockedExchange(&g_shared->backend, (long)inj.backend);
    printf("[+] 命令环已就绪，等待 DLL 指令（backend=%s）\n", backendArg.c_str());

    // 7) 主循环：消费指令 + 心跳
    unsigned long long lastBeat = 0;
    for (;;)
    {
        const unsigned long long tickNow = GetTickCount64();
        g_ac.Tick(inj, tickNow, g_shared);   // 外置 AC 引擎：点击调度在这里产出
        ProcessRing(inj, gameWnd);

        const unsigned long long now = GetTickCount64();
        if (now - lastBeat >= 100)
        {
            lastBeat = now;
            InterlockedExchange(&g_shared->aliveMs, (long)now);
            if (!IsWindow(gameWnd))
            {
                printf("[*] 游戏窗口已关闭，退出。\n");
                break;
            }
        }
        Sleep(1);   // 1ms 轮询，保证转头/连点的时序精度
    }

    return 0;
}