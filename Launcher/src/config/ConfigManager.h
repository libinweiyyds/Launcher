#pragma once

#include <string>
#include <vector>
#include <Windows.h>

// WebSocket 连接配置
struct WebsocketConfig {
    std::string address = "localhost";
    int port = 8080;
    std::string group = "launcher";
    int id = 1;
};

// 版本信息
struct VersionInfo {
    std::string current;
    std::vector<std::string> history;
};

// 软件配置
struct SoftwareConfig {
    std::string name = "111";
    std::string exeName = "start.exe";
};

// 配置管理器：读取 JSON 配置文件，支持热加载
// 使用 FindFirstChangeNotification 监听文件变更
class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    // 加载配置文件，返回是否成功
    // filePath: 配置文件完整路径
    bool load(const std::string& filePath);

    // 获取 WebSocket 配置
    const WebsocketConfig& getWebsocket() const;

    // 获取版本信息
    const VersionInfo& getVersion() const;

    // 获取软件配置
    const SoftwareConfig& getSoftware() const;

    // 启动文件变更监听，返回监听句柄（供 WaitForMultipleObjects 使用）
    // 失败返回 nullptr
    HANDLE startWatching();

    // 停止文件变更监听
    void stopWatching();

    // 检查文件是否有变更并重新加载
    // 返回 true 表示配置已变更（target.path 不同）
    bool checkAndReload();

    // 获取当前加载的文件路径
    const std::string& getFilePath() const;

private:
    // 解析 JSON 内容
    bool parseJson(const std::string& jsonContent);

    std::string m_filePath;               // 配置文件完整路径
    WebsocketConfig m_websocket;          // WebSocket 配置
    VersionInfo m_version;                // 版本信息
    SoftwareConfig m_software;            // 软件配置
    HANDLE m_hChangeNotify = nullptr;     // FindFirstChangeNotification 句柄
};
