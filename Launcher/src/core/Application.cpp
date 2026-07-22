#include "Application.h"
#include "../logger/Logger.h"
#include <filesystem>
#include <shellapi.h>

namespace fs = std::filesystem;

// 静态成员初始化
Application* Application::s_instance = nullptr;

// 获取全局唯一实例
Application& Application::instance() {
    static Application inst;
    return inst;
}

// 析构：确保资源释放
Application::~Application() {
    shutdown();
}

// 程序入口
// 执行初始化 → 主循环 → 清理的完整生命周期
int Application::run(HINSTANCE hInstance) {
    // 注册控制台事件处理器（响应系统关机、用户注销等）
    s_instance = this;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // 屏蔽可能的异常退出，确保日志能记录退出原因
    if (!init()) {
        LOG_ERROR("初始化失败，程序即将退出");
        shutdown();
        return 1;
    }

    LOG_INFO("Launcher 启动完成（PID: %lu）", GetCurrentProcessId());

    // 进入主循环，等待退出信号
    mainLoop();

    // 执行清理流程
    shutdown();
    return 0;
}

// 初始化阶段
// 按顺序执行：日志初始化 → 单实例检查 → 启动目标进程
// 任一步骤失败则返回 false
bool Application::init() {
    // 1. 初始化日志系统
    // 使用相对路径 ./logs，后续阶段改为从配置文件读取
    if (!Logger::instance().init("./logs")) {
        // 日志初始化失败，无法记录后续错误
        return false;
    }

    LOG_INFO("日志系统初始化完成");

    // 2. 单实例检查
    // 如果已有实例在运行，则拒绝启动
    m_hMutex = CreateMutexW(nullptr, TRUE, L"Global\\Launcher_SingleInstance");
    if (m_hMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_ERROR("检测到已有 Launcher 实例在运行，本次启动取消");
        if (m_hMutex) {
            CloseHandle(m_hMutex);
            m_hMutex = nullptr;
        }
        return false;
    }

    LOG_INFO("单实例检查通过");

    // 3. 启动目标 Qt 程序
    if (!startTargetProcess()) {
        LOG_ERROR("目标程序启动失败");
        return false;
    }

    LOG_INFO("目标程序已启动（PID: %lu）", m_dwProcessId);

    m_running = true;
    return true;
}

// 主循环：等待退出事件
// 当 m_running 变为 false 时退出循环
void Application::mainLoop() {
    LOG_INFO("进入主循环");
    // Simple polling loop: sleep to avoid busy-waiting
    // Proper event-based waiting will be added when WebSocket is integrated
    while (m_running) {
        // 检查子进程是否仍在运行
        if (m_hProcess) {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(m_hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                LOG_WARN("目标程序已退出（退出码: %lu）", exitCode);
                // Phase 2 将在此处实现自动重启逻辑
                CloseHandle(m_hProcess);
                m_hProcess = nullptr;
                m_dwProcessId = 0;
            }
        }
        Sleep(1000);
    }
    LOG_INFO("主循环已退出");
}

// 清理阶段
// 逆序释放资源：先终止子进程，再关闭日志
void Application::shutdown() {
    m_running = false;

    // 1. 终止目标进程
    stopTargetProcess();

    // 2. 释放单实例互斥体
    if (m_hMutex) {
        ReleaseMutex(m_hMutex);
        CloseHandle(m_hMutex);
        m_hMutex = nullptr;
    }

    // 3. 关闭日志系统（最后关闭，确保前面的日志都能写入）
    Logger::instance().shutdown();

    s_instance = nullptr;
}

// 启动被管理的目标程序
// 从可执行文件所在目录向上逐级查找 testStart/DMSProcess.exe
// 使用 CreateProcess 以独立进程方式启动，隐藏其主窗口
bool Application::startTargetProcess() {
    // 获取当前可执行文件所在目录
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    fs::path exeDir = fs::path(exePath).parent_path();

    // 从 exe 所在目录向上逐级查找 testStart/DMSProcess.exe
    // 避免硬编码路径深度（Debug 和 x64/Debug 深度不同）
    fs::path targetExe;
    fs::path searchDir = exeDir;
    for (int i = 0; i < 5; ++i) {
        fs::path candidate = searchDir / L"testStart" / L"DMSProcess.exe";
        if (fs::exists(candidate)) {
            targetExe = fs::canonical(candidate);
            break;
        }
        // 到达根目录则停止
        fs::path parent = searchDir.parent_path();
        if (parent == searchDir) break;
        searchDir = parent;
    }

    if (targetExe.empty()) {
        LOG_ERROR("未找到目标程序（已从 %ls 向上搜索 5 级）", exeDir.c_str());
        return false;
    }

    LOG_INFO("正在启动目标程序: %ls", targetExe.c_str());

    // 设置启动信息：隐藏窗口方式启动
    // 目标程序的工作目录设为其所在目录，确保它找到自己的 DLL 和配置
    fs::path targetDir = fs::path(targetExe).parent_path();

    STARTUPINFOW si = { sizeof(si) };
    // 不设置 STARTF_USESHOWWINDOW，让目标程序使用自己的默认显示方式
    PROCESS_INFORMATION pi = {};

    BOOL result = CreateProcessW(
        targetExe.c_str(),      // 可执行文件路径
        nullptr,                // 命令行参数（不附加额外参数）
        nullptr,                // 进程安全属性
        nullptr,                // 线程安全属性
        FALSE,                  // 不继承句柄
        0,                      // 不附加特殊标志，目标程序正常显示窗口
        nullptr,                // 环境变量（继承当前进程）
        targetDir.c_str(),      // 工作目录设为目标程序所在目录
        &si,
        &pi
    );

    if (!result) {
        DWORD err = GetLastError();
        LOG_ERROR("CreateProcess 失败（错误码: %lu）", err);
        return false;
    }

    // 保存子进程句柄和 PID
    m_hProcess = pi.hProcess;
    m_dwProcessId = pi.dwProcessId;

    // 不需要子进程的主线程句柄，立即关闭
    CloseHandle(pi.hThread);

    LOG_INFO("目标程序启动成功（PID: %lu）", m_dwProcessId);

    return true;
}

// 终止目标进程
// 先尝试温和关闭，超时后强制终止
void Application::stopTargetProcess() {
    if (!m_hProcess) return;

    LOG_INFO("正在终止目标程序（PID: %lu）", m_dwProcessId);

    // 检查进程是否已经退出
    DWORD exitCode = 0;
    if (GetExitCodeProcess(m_hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
        // 进程已经不存在了
        LOG_INFO("目标程序已自行退出（退出码: %lu）", exitCode);
    } else {
        // 进程仍在运行，强制终止
        // Phase 2 将改为先发送 WM_CLOSE 温和退出，超时后再强制终止
        if (TerminateProcess(m_hProcess, 0)) {
            LOG_INFO("目标程序已被终止");
        } else {
            DWORD err = GetLastError();
            LOG_ERROR("终止目标程序失败（错误码: %lu）", err);
        }
    }

    CloseHandle(m_hProcess);
    m_hProcess = nullptr;
    m_dwProcessId = 0;
}

// 控制台事件处理器
// 处理系统关机、用户注销等事件，确保优雅退出
// 注意：Windows 子系统下 SetConsoleCtrlHandler 仍可接收部分事件（关机/注销）
BOOL WINAPI Application::ctrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:           // Ctrl+C
        case CTRL_BREAK_EVENT:       // Ctrl+Break
        case CTRL_CLOSE_EVENT:       // 控制台窗口关闭
        case CTRL_LOGOFF_EVENT:      // 用户注销
        case CTRL_SHUTDOWN_EVENT:    // 系统关机
            LOG_INFO("收到退出信号（类型: %lu），正在退出...", ctrlType);
            if (s_instance) {
                s_instance->m_running = false;
            }
            return TRUE;
        default:
            return FALSE;
    }
}
