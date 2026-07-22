#pragma once

#include <Windows.h>
#include <string>
#include <memory>

class ProcessManager;
class ConfigManager;
class IpcBridge;

// 应用程序生命周期管理器（单例）
// 负责：初始化日志 → 单实例检查 → 加载配置 → 启动子进程 → IPC服务 → 主循环 → 优雅退出
// 主循环同时监听子进程退出、配置文件变更、IPC 停止事件
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

    // 初始化阶段：日志 → 单实例 → 配置 → 启动子进程 → IPC
    bool init();

    // 主循环：WaitForMultipleObjects 等待子进程退出 + 配置变更 + IPC 停止
    void mainLoop();

    // 清理阶段：终止子进程 → 释放资源 → 关闭日志
    void shutdown();

    // 查找配置文件路径（从 exe 目录向上搜索 Config/config.json）
    std::wstring findConfigPath();

    // 解析目标程序路径（向上搜索匹配相对路径）
    std::wstring resolveTargetPath(const std::string& relativePath);

    // IPC 消息回调
    void onIpcMessage(const std::string& message);

    // 控制台事件回调（处理系统关机、用户注销等）
    static BOOL WINAPI ctrlHandler(DWORD ctrlType);

    // 静态指针，用于在控制台回调中访问实例
    static Application* s_instance;

    bool m_running = false;
    HANDLE m_hMutex = nullptr;                                  // 单实例互斥体句柄
    std::unique_ptr<ConfigManager> m_configMgr;                 // 配置管理器
    std::unique_ptr<ProcessManager> m_processMgr;               // 进程管理器
    std::unique_ptr<IpcBridge> m_ipcBridge;                     // IPC 通信桥接
    std::wstring m_currentTargetPath;                           // 当前生效的目标程序路径
};
