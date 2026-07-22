#include "IpcBridge.h"
#include "../logger/Logger.h"
#include "../third-party/ipc/include/ipc.h"

IpcBridge::IpcBridge() = default;

// 析构：确保资源释放
IpcBridge::~IpcBridge() {
    stop();
}

// 启动 IPC 服务端
bool IpcBridge::start(const std::string& pipeName) {
    if (m_running) {
        LOG_WARN("IPC 服务已在运行中");
        return false;
    }

    LOG_INFO("正在创建 IPC 服务端: %s", pipeName.c_str());

    // 创建 Named Pipe 服务端
    m_hServer = ipc_server_create(pipeName.c_str());
    if (!m_hServer) {
        LOG_ERROR("IPC 服务端创建失败: %s", pipeName.c_str());
        return false;
    }

    // 创建停止事件（手动重置）
    m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_hStopEvent) {
        LOG_ERROR("IPC 停止事件创建失败");
        ipc_close(m_hServer);
        m_hServer = nullptr;
        return false;
    }

    // 启动接收线程
    m_running = true;
    m_hThread = CreateThread(nullptr, 0, receiveThreadProc, this, 0, nullptr);
    if (!m_hThread) {
        LOG_ERROR("IPC 接收线程创建失败");
        m_running = false;
        CloseHandle(m_hStopEvent);
        m_hStopEvent = nullptr;
        ipc_close(m_hServer);
        m_hServer = nullptr;
        return false;
    }

    LOG_INFO("IPC 服务已启动，等待客户端连接...");
    return true;
}

// 停止 IPC 服务
void IpcBridge::stop() {
    if (!m_running) return;

    LOG_INFO("正在停止 IPC 服务...");
    m_running = false;

    // 通知主循环
    if (m_hStopEvent) {
        SetEvent(m_hStopEvent);
    }

    // 等待接收线程退出
    if (m_hThread) {
        WaitForSingleObject(m_hThread, 3000);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }

    // 关闭 IPC
    if (m_hServer) {
        ipc_close(m_hServer);
        m_hServer = nullptr;
    }

    // 释放事件
    if (m_hStopEvent) {
        CloseHandle(m_hStopEvent);
        m_hStopEvent = nullptr;
    }

    m_connected = false;
    LOG_INFO("IPC 服务已停止");
}

// 发送 JSON 消息到客户端
bool IpcBridge::send(const std::string& jsonMsg) {
    if (!m_hServer) {
        LOG_WARN("IPC 未启动，无法发送消息");
        return false;
    }

    if (!m_connected) {
        LOG_WARN("IPC 客户端未连接，无法发送消息");
        return false;
    }

    int sent = ipc_send(m_hServer, jsonMsg.c_str(), static_cast<int>(jsonMsg.size()));
    if (sent < 0) {
        LOG_ERROR("IPC 发送失败");
        return false;
    }

    LOG_DEBUG("IPC 发送成功: %d 字节", sent);
    return true;
}

// 客户端连接状态
bool IpcBridge::isConnected() const {
    return m_connected;
}

// 设置消息回调
void IpcBridge::setMessageCallback(MessageCallback cb) {
    m_msgCallback = std::move(cb);
}

// 获取停止事件句柄
HANDLE IpcBridge::getStopEvent() const {
    return m_hStopEvent;
}

// 接收线程入口
DWORD WINAPI IpcBridge::receiveThreadProc(LPVOID param) {
    auto* self = static_cast<IpcBridge*>(param);
    self->receiveLoop();
    return 0;
}

// 接收循环：阻塞等待客户端数据
void IpcBridge::receiveLoop() {
    char buffer[4096];

    while (m_running) {
        // 阻塞等待接收数据
        int received = ipc_receive(m_hServer, buffer, sizeof(buffer) - 1);

        if (!m_running) break;

        if (received > 0) {
            // 收到数据
            buffer[received] = '\0';

            if (!m_connected) {
                m_connected = true;
                LOG_INFO("IPC 客户端已连接");
            }

            std::string msg(buffer);
            LOG_INFO("IPC 收到消息: %s", msg.c_str());

            if (m_msgCallback) {
                m_msgCallback(msg);
            }
        } else if (received == 0) {
            // 客户端断开
            if (m_connected) {
                m_connected = false;
                LOG_INFO("IPC 客户端已断开，等待重连...");
            }
        } else {
            // 接收错误
            if (m_connected) {
                m_connected = false;
                LOG_ERROR("IPC 接收错误，等待重连...");
            }
            // 短暂休眠避免错误时忙等
            Sleep(100);
        }
    }
}
