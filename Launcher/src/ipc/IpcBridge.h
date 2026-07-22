#pragma once

#include <string>
#include <functional>
#include <Windows.h>

// 前向声明 IPC 库的类型
typedef void* IPC_HANDLE;

// IPC 通信桥接类：封装 third-party/ipc 库
// Launcher 作为 Named Pipe 服务端，等待客户端连接
// 在独立线程中阻塞接收消息，通过回调通知上层
class IpcBridge {
public:
    IpcBridge();
    ~IpcBridge();

    // 启动 IPC 服务端
    // pipeName: 管道名称（如 "MyService"），底层对应 \\.\pipe\MyService
    bool start(const std::string& pipeName);

    // 停止 IPC 服务
    void stop();

    // 发送 JSON 消息到已连接的客户端
    bool send(const std::string& jsonMsg);

    // 客户端是否已连接
    bool isConnected() const;

    // 设置消息接收回调（回调在接收线程中触发）
    using MessageCallback = std::function<void(const std::string& jsonMsg)>;
    void setMessageCallback(MessageCallback cb);

    // 获取停止事件句柄（供 mainLoop 的 WaitForMultipleObjects 使用）
    HANDLE getStopEvent() const;

private:
    // 接收线程入口
    static DWORD WINAPI receiveThreadProc(LPVOID param);

    // 接收循环
    void receiveLoop();

    IPC_HANDLE m_hServer = nullptr;       // IPC 服务端句柄
    HANDLE m_hThread = nullptr;           // 接收线程句柄
    HANDLE m_hStopEvent = nullptr;        // 停止事件（手动重置）
    MessageCallback m_msgCallback;        // 消息接收回调
    bool m_running = false;               // 运行标志
    bool m_connected = false;             // 客户端连接状态
};
