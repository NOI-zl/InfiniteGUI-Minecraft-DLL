#pragma once

#include <Windows.h>

// ============================================================================
// AcInputLink.h —— DLL 与「外置注入器」之间的共享内存 ABI
//
// 为什么要有这一层：
//   SendInput / mouse_event 注入的输入会被系统打上 LLMHF_INJECTED 标记，
//   游戏和反作弊一眼就能看出"这不是人手的输入"。AC 原本的隐藏要求
//   （伪造输入、进程名随机、内存不可读）和 Telly 的点击/转头都要绕过这一点，
//   而 DLL 跑在游戏进程里做不到——所以：DLL 只把"要干什么"写进共享内存，
//   由**外置程序**用真正隐蔽的后端完成注入。
//
// 数据流：
//   DLL(游戏进程) --写 ring--> 共享内存 <--读 ring-- 外置注入器 --> 内核/HID/驱动
//
// 命名：Local\ACTG_IN_<pid> / Local\ACTG_EV_<pid>（pid = 游戏进程的 pid，也就是 DLL 所在进程）
// 该 ABI 同时被 overlay 与 injector 使用，改动必须两边同步（workflow 会校验一致性）。
// ============================================================================

namespace AcInput
{
    constexpr unsigned kMagic   = 0x47544341u;  // 'ACTG'
    constexpr unsigned kVersion = 1u;
    constexpr unsigned kRingSize = 4096u;       // 必须是 2 的幂

    enum class Cmd : unsigned
    {
        None = 0,
        MouseMove = 1,   // dx, dy
        MouseDown = 2,   // vk
        MouseUp   = 3,   // vk
        KeyDown   = 4,   // vk
        KeyUp     = 5,   // vk
        KeyClick  = 6,   // vk, holdMs
        MouseClick= 7,   // vk, holdMs
        Stop      = 8,
        UseHold   = 9,   // vk=RButton, holdMs=1 按住 / 0 抬起（Telly 的放置状态）
        PlaceClick= 10   // vk=RButton, holdMs=单次点击的按住时长（蹲起塔/放置）
    };

    // 外置 AC 引擎配置：DLL 写、注入器读（seq 变化检测）。
    // engineOn=0 时注入器只做"转发"，点击调度仍在 DLL 里；
    // engineOn=1 时点击调度搬进注入器（外置 AC），DLL 侧不再自己发点击。
    struct AcProfile
    {
        unsigned seq;            // 变更计数（用 InterlockedIncrement 写）
        unsigned engineOn;       // 1 = 外置 AC 引擎接管点击
        unsigned mode;           // 0 = 模式A(1+N) 1 = 连点器
        unsigned useAsClicks;    // 1 = Telly 的 use 状态转成按 CPS 的点击
        unsigned leftEnabled;
        unsigned rightEnabled;
        unsigned leftCps;
        unsigned rightCps;
        unsigned jitterPct;
        unsigned holdMin;
        unsigned holdMax;
        unsigned extra;          // 模式A 补点次数
        unsigned longMs;         // 模式A 长按判定
        unsigned reserved[3];
    };

    // 外置注入器实际使用的注入后端（决定"隐藏"到什么程度）
    enum class Backend : long
    {
        Unknown     = 0,
        SendInput   = 1,  // 系统级，带 INJECTED 标记（最容易被识别）
        MouseEvent  = 2,  // 同上，老 API
        PostMessage = 3,  // 直接塞窗口消息，部分客户端不认
        HidDriver   = 4,  // 内核过滤驱动（Interception 式）：无 INJECTED 标记，等同物理设备
        VulnDriver  = 5,  // 利用已签名驱动（BYOVD）写内核鼠标队列：无 INJECTED 标记
        HidSerial   = 6,  // 外置硬件 HID（Arduino / Kmbox 之类）：物理层，不可分辨
        UsermodeHide= 7   // SendInput + 游戏进程内过滤（RawInput / WndProc），只骗游戏
    };

    struct Slot
    {
        volatile unsigned seq;    // 生产者写完 payload 后自增；消费者比对前后 seq
        unsigned cmd;
        unsigned vk;
        int      dx;
        int      dy;
        unsigned holdMs;
        unsigned reserved;
    };

    struct Shared
    {
        unsigned magic;
        unsigned version;
        volatile long head;        // 生产者（DLL）写指针
        volatile long tail;        // 消费者（注入器）读指针
        volatile long aliveMs;     // 注入器心跳
        volatile long backend;     // 当前后端（Backend）
        volatile long injected;    // 已注入计数
        volatile long dropped;     // 丢包计数（ring 满）
        volatile long hidden;      // 已启用隐藏手段的位掩码
        volatile long clicksDone;  // 外置 AC 引擎发出的点击数
        volatile long useHeld;     // 当前 use（右键）是否处于按住状态
        AcProfile     ac;
        unsigned char pad[32];
        Slot ring[kRingSize];
    };
}