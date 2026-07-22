#pragma once

#include <string>
#include <Windows.h>

// 目标程序配置
struct TargetConfig {
    std::string path;          // 目标程序相对路径（如 testStart/DMSProcess.exe）
    std::string workingDir;    // 工作目录，空则使用目标程序所在目录
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

    // 获取目标程序配置
    const TargetConfig& getTarget() const;

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
    TargetConfig m_target;                // 当前目标程序配置
    TargetConfig m_prevTarget;            // 变更前的配置（用于对比）
    HANDLE m_hChangeNotify = nullptr;     // FindFirstChangeNotification 句柄
};
