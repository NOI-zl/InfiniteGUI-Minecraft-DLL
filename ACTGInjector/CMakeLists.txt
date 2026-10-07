cmake_minimum_required(VERSION 3.20)
project(ACTGInjector CXX)

# =============================================================================
# ACTGInjector —— 外置隐藏/绕过/注入器（独立运行的程序，不属于 DLL 工程）
#
# 它负责 DLL 做不到的事：
#   * 随机进程名、自动提权、用户态隐藏（PEB 清空 / 缓解策略 / 字符串混淆）
#   * 用"无 INJECTED 标记"的后端发出输入（内核过滤驱动 / BYOVD / 外置硬件 HID）
#   * 注入 InfiniteGUI-DLL.dll 到 Minecraft，并代 DLL 执行所有输入注入
#   * 外置 AC 引擎：点击调度（CPS/抖动/按住时长/左右键）跑在这里
#
# 编译（x64 / Release）：
#   cmake -S ACTGInjector -B build-injector -A x64
#   cmake --build build-injector --config Release
# 产物：build-injector/out/ACTGInjector.exe
# =============================================================================

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "必须 x64（游戏是 64 位，注入器也必须 64 位）")
endif()

add_executable(ACTGInjector main.cpp)

target_compile_features(ACTGInjector PRIVATE cxx_std_17)
target_compile_definitions(ACTGInjector PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# 共享 ABI 头（AcInputLink.h）单一来源：两种仓库布局都兼容
set(AC_ABI_DIR "")
foreach(cand "../InfiniteGUI-DLL" "../overlay/InfiniteGUI-DLL")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${cand}/AcInputLink.h")
        set(AC_ABI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${cand}")
        break()
    endif()
endforeach()

if(AC_ABI_DIR STREQUAL "")
    message(FATAL_ERROR "找不到 AcInputLink.h（应在 InfiniteGUI-DLL/ 或 overlay/InfiniteGUI-DLL/ 下）")
endif()
message(STATUS "ABI 头目录: ${AC_ABI_DIR}")

target_include_directories(ACTGInjector PRIVATE ${AC_ABI_DIR})

if(MSVC)
    target_compile_options(ACTGInjector PRIVATE /W3 /utf-8 /EHsc)
    set_property(TARGET ACTGInjector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(ACTGInjector PRIVATE advapi32 shell32 user32)

set_target_properties(ACTGInjector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/out
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
)