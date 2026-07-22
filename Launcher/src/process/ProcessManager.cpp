#include "ProcessManager.h"
#include "../logger/Logger.h"
#include <filesystem>
#include <tlhelp32.h>

namespace fs = std::filesystem;

// EnumWindows 回调上下文：用于查找特定 PID 的窗口并发送 WM_CLOSE
struct EnumWindowsContext {
    DWORD targetPid;
    bool found;
};

// EnumWindows 回调：查找属于目标进程的顶层窗口
static BOOL CALLBACK findWindowCallback(HWND hWnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumWindowsContext*>(lParam);

    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);

    if (pid == ctx->targetPid && IsWindowVisible(hWnd)) {
        // 找到目标进程的可见窗口，发送 WM_CLOSE
        PostMessageW(hWnd, WM_CLOSE, 0, 0);
        ctx->found = true;
    }

    return TRUE;  // 继续枚举
}

ProcessManager::ProcessManager() = default;

// 析构：确保资源释放
ProcessManager::~ProcessManager() {
    stop();
    if (m_hJob) {
        CloseHandle(m_hJob);
        m_hJob = nullptr;
    }
}

// 启动目标进程
bool ProcessManager::start(const std::wstring& exePath, const std::wstring& workingDir) {
    // 如果已有进程在运行，先停止
    if (isRunning()) {
        LOG_WARN("已有子进程在运行（PID: %lu），先终止再启动", m_dwPid);
        stop();
    }

    // 创建 Job Object（如果尚未创建）
    if (!m_hJob) {
        if (!createJobObject()) {
            return false;
        }
    }

    // 启动进程
    if (!launchProcess(exePath, workingDir)) {
        return false;
    }

    LOG_INFO("子进程启动成功（PID: %lu）", m_dwPid);
    return true;
}

// 停止目标进程
// 温和退出（WM_CLOSE → 等待 timeoutMs）→ 强制终止
void ProcessManager::stop(DWORD timeoutMs) {
    if (!m_hProcess) return;

    LOG_INFO("正在停止子进程（PID: %lu）", m_dwPid);

    // 先检查进程是否已经退出
    DWORD exitCode = 0;
    if (GetExitCodeProcess(m_hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
        LOG_INFO("子进程已自行退出（退出码: %lu）", exitCode);
        cleanupProcessHandle();
        return;
    }

    // 发送 WM_CLOSE 温和退出
    sendCloseMessage();

    // 等待进程退出
    DWORD result = WaitForSingleObject(m_hProcess, timeoutMs);
    if (result == WAIT_OBJECT_0) {
        GetExitCodeProcess(m_hProcess, &exitCode);
        LOG_INFO("子进程已优雅退出（退出码: %lu）", exitCode);
    } else {
        // 超时，强制终止
        LOG_WARN("子进程未在 %lu ms 内响应，强制终止", timeoutMs);
        if (TerminateProcess(m_hProcess, 1)) {
            LOG_INFO("子进程已被强制终止");
        } else {
            DWORD err = GetLastError();
            LOG_ERROR("强制终止子进程失败（错误码: %lu）", err);
        }
    }

    cleanupProcessHandle();
}

// 检查进程是否在运行
bool ProcessManager::isRunning() const {
    if (!m_hProcess) return false;

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_hProcess, &exitCode)) return false;

    return exitCode == STILL_ACTIVE;
}

// 获取进程句柄
HANDLE ProcessManager::getProcessHandle() const {
    return m_hProcess;
}

// 获取 PID
DWORD ProcessManager::getPid() const {
    return m_dwPid;
}

// 创建 Job Object
// 设置 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE：
// Launcher 被杀后，Windows 自动终止 Job 内所有子进程
bool ProcessManager::createJobObject() {
    m_hJob = CreateJobObjectW(nullptr, nullptr);
    if (!m_hJob) {
        DWORD err = GetLastError();
        LOG_ERROR("创建 Job Object 失败（错误码: %lu）", err);
        return false;
    }

    // 设置 KILL_ON_JOB_CLOSE 标志
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(m_hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        DWORD err = GetLastError();
        LOG_ERROR("设置 Job Object 属性失败（错误码: %lu）", err);
        CloseHandle(m_hJob);
        m_hJob = nullptr;
        return false;
    }

    LOG_INFO("Job Object 已创建（KILL_ON_JOB_CLOSE）");
    return true;
}

// 将进程加入 Job Object
bool ProcessManager::assignToJob(HANDLE hProcess) {
    if (!m_hJob) return false;

    if (!AssignProcessToJobObject(m_hJob, hProcess)) {
        DWORD err = GetLastError();
        LOG_ERROR("将进程加入 Job Object 失败（错误码: %lu）", err);
        return false;
    }

    return true;
}

// 启动子进程
bool ProcessManager::launchProcess(const std::wstring& exePath, const std::wstring& workingDir) {
    LOG_INFO("正在启动子进程: %ls", exePath.c_str());

    // 确定工作目录
    std::wstring actualWorkingDir = workingDir;
    if (actualWorkingDir.empty()) {
        // 默认使用 exe 所在目录作为工作目录
        fs::path exeDir = fs::path(exePath).parent_path();
        actualWorkingDir = exeDir.wstring();
    }

    // 设置启动信息：不强制隐藏，让目标程序自己决定窗口显示
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    BOOL result = CreateProcessW(
        exePath.c_str(),
        nullptr,                // 命令行参数
        nullptr,                // 进程安全属性
        nullptr,                // 线程安全属性
        FALSE,                  // 不继承句柄
        0,                      // 无特殊标志
        nullptr,                // 环境变量
        actualWorkingDir.c_str(),
        &si,
        &pi
    );

    if (!result) {
        DWORD err = GetLastError();
        LOG_ERROR("CreateProcess 失败（错误码: %lu）", err);
        return false;
    }

    // 将进程加入 Job Object
    assignToJob(pi.hProcess);

    // 保存句柄和 PID
    m_hProcess = pi.hProcess;
    m_dwPid = pi.dwProcessId;

    // 关闭主线程句柄（不需要）
    CloseHandle(pi.hThread);

    return true;
}

// 清理进程句柄
void ProcessManager::cleanupProcessHandle() {
    if (m_hProcess) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }
    m_dwPid = 0;
}

// 查找目标进程的顶层窗口并发送 WM_CLOSE
void ProcessManager::sendCloseMessage() {
    if (!m_hProcess || m_dwPid == 0) return;

    EnumWindowsContext ctx = { m_dwPid, false };
    EnumWindows(findWindowCallback, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found) {
        LOG_INFO("已向子进程窗口发送 WM_CLOSE（PID: %lu）", m_dwPid);
    } else {
        LOG_DEBUG("未找到子进程的可见窗口，将直接终止（PID: %lu）", m_dwPid);
    }
}
