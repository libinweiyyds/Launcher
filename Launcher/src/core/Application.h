#pragma once

#include <Windows.h>
#include <string>

// 应用程序生命周期管理器（单例）
// 负责：初始化日志 → 单实例检查 → 启动子进程 → 主循环等待 → 优雅退出
class Application {
public:
    // 获取全局唯一实例
    static Application& instance();

    // 启动应用程序，进入主循环
    // 返回值：进程退出码（0 正常，非 0 异常）
    int run(HINSTANCE hInstance);

private:
    Application() = default;
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 初始化阶段：日志、单实例互斥体、启动子进程
    bool init();

    // 主消息循环：等待退出信号
    void mainLoop();

    // 清理阶段：终止子进程、关闭日志
    void shutdown();

    // 启动被管理的目标程序（testStart/DMSProcess.exe）
    bool startTargetProcess();

    // 终止被管理的目标程序
    void stopTargetProcess();

    // 控制台事件回调（处理系统关机、用户注销等）
    static BOOL WINAPI ctrlHandler(DWORD ctrlType);

    // 静态指针，用于在控制台回调中访问实例
    static Application* s_instance;

    bool m_running = false;         // 主循环运行标志
    HANDLE m_hProcess = nullptr;    // 子进程句柄
    DWORD m_dwProcessId = 0;        // 子进程 PID
    HANDLE m_hMutex = nullptr;      // 单实例互斥体句柄
};
