#pragma once

#include <string>
#include <vector>
#include <Windows.h>

// 服务器配置（HTTP API 与 WebSocket 共用同一服务器地址/端口；id 是设备唯一标识，UUID 格式）
struct WebsocketConfig {
    std::string address = "localhost";
    int port = 8080;
    std::string group = "launcher";
    std::string id;     // 设备 UUID，首次启动自动生成并写盘，保证全局唯一
};

// 版本信息
struct VersionInfo {
    std::string current;
    std::vector<std::string> history;
};

// 软件配置
struct SoftwareConfig {
    std::string softwareName = "";     // 中文产品名（用于服务端接口请求）
    std::string exeName = "start.exe"; // 被管理软件的可执行文件名
    std::string sha256 = "";           // 远程配置文件 SHA256（基准 Hash）
    std::string configId = "";         // 服务端配置记录 ID
    std::string localFilePath = "";    // 管理软件本地配置文件路径（例：./versions/{ver}/config/config.json）
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

    // 读取 software.sha256（启动阶段只读，不写入）
    const std::string& getSoftwareSha256() const;

    // 写入 software.sha256（仅退出上传成功后调用，仅更新内存）
    // 调用方需自行触发 writeConfigToDiskPublic() 写盘
    void setSoftwareSha256(const std::string& hash);

    // 写入 software.configId 并立即写盘（启动器阶段可用，标识当前远程配置记录）
    void setSoftwareConfigId(const std::string& configId);

    // 写入 software.localFilePath 并立即写盘（记录管理软件配置文件本地路径）
    void setSoftwareLocalFilePath(const std::string& path);

    // 公开接口：在 setSoftwareSha256 后由调用方触发写盘
    bool writeConfigToDiskPublic() { return writeConfigToDisk(false); }

    // 更新 version.current + version.history 并写盘（升级时使用）
    bool setCurrentVersion(const std::string& newVer,
                            const std::vector<std::string>& newHistory);

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

    // 把当前内存配置写到磁盘（创建文件或合并补全缺失字段）
    // 已存在的非空段保持不动；缺失的 server 段用默认 localhost:8080 写入
    // 返回 true 表示写盘成功（或文件已存在无需写）
    bool writeConfigToDisk(bool createIfMissing);

    // 默认服务器配置常量
    static constexpr const char* kDefaultAddress = "localhost";
    static constexpr int kDefaultPort = 8080;

    std::string m_filePath;               // 配置文件完整路径
    WebsocketConfig m_websocket;          // WebSocket 配置
    VersionInfo m_version;                // 版本信息
    SoftwareConfig m_software;            // 软件配置
    HANDLE m_hChangeNotify = nullptr;     // FindFirstChangeNotification 句柄
};
