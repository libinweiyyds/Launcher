#pragma once

#include <Windows.h>
#include <string>

// 进程管理器：负责子进程的启动、监控、优雅终止
// 使用 Job Object 确保 Launcher 被杀后子进程自动终止
class ProcessManager {
public:
    ProcessManager();
    ~ProcessManager();

    // 启动目标进程
    // exePath: 可执行文件完整路径
    // workingDir: 工作目录，空则使用 exe 所在目录
    bool start(const std::wstring& exePath, const std::wstring& workingDir);

    // 停止目标进程
    // 先发送 WM_CLOSE 温和退出，超时后强制终止
    void stop(DWORD timeoutMs = 5000);

    // 检查进程是否正在运行
    bool isRunning() const;

    // 获取进程句柄（供 WaitForSingleObject 使用）
    HANDLE getProcessHandle() const;

    // 获取进程 PID
    DWORD getPid() const;

private:
    // 创建进程内部实现
    bool launchProcess(const std::wstring& exePath, const std::wstring& workingDir);

    // 创建 Job Object 并设置 KILL_ON_JOB_CLOSE
    bool createJobObject();

    // 将进程加入 Job Object
    bool assignToJob(HANDLE hProcess);

    // 查找目标进程的顶层窗口并发送 WM_CLOSE
    void sendCloseMessage();

    // 清理进程句柄
    void cleanupProcessHandle();

    HANDLE m_hJob = nullptr;          // Job Object 句柄
    HANDLE m_hProcess = nullptr;      // 子进程句柄
    DWORD m_dwPid = 0;               // 子进程 PID
};
