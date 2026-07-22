#include "ConfigManager.h"
#include "../logger/Logger.h"
#include <json/json.h>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

ConfigManager::ConfigManager() = default;

ConfigManager::~ConfigManager() {
    stopWatching();
}

// 加载配置文件
// 文件不存在时使用默认配置，记录警告
bool ConfigManager::load(const std::string& filePath) {
    m_filePath = filePath;

    // 尝试读取文件内容
    std::ifstream file(m_filePath);
    if (!file.is_open()) {
        LOG_WARN("配置文件不存在: %s，使用默认配置", m_filePath.c_str());
        // 使用默认配置：testStart/DMSProcess.exe
        m_target.path = "testStart/DMSProcess.exe";
        m_target.workingDir = "";
        m_prevTarget = m_target;
        return true;
    }

    // 读取整个文件到字符串
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // 确保不是空文件
    if (content.empty()) {
        LOG_WARN("配置文件为空: %s，使用默认配置", m_filePath.c_str());
        m_target.path = "testStart/DMSProcess.exe";
        m_target.workingDir = "";
        m_prevTarget = m_target;
        return true;
    }

    // 解析 JSON
    if (!parseJson(content)) {
        LOG_ERROR("配置文件 JSON 解析失败: %s，使用默认配置", m_filePath.c_str());
        m_target.path = "testStart/DMSProcess.exe";
        m_target.workingDir = "";
        m_prevTarget = m_target;
        return false;
    }

    // 保存当前配置作为"变更前"的基线
    m_prevTarget = m_target;

    LOG_INFO("配置文件加载成功: %s (target=%s)", m_filePath.c_str(), m_target.path.c_str());
    return true;
}

// 解析 JSON 内容
bool ConfigManager::parseJson(const std::string& jsonContent) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;

    std::istringstream stream(jsonContent);
    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        LOG_ERROR("JSON 解析错误: %s", errors.c_str());
        return false;
    }

    // 读取 target.path
    if (root.isMember("target") && root["target"].isObject()) {
        const Json::Value& target = root["target"];
        if (target.isMember("path") && target["path"].isString()) {
            m_target.path = target["path"].asString();
        }
        if (target.isMember("workingDir") && target["workingDir"].isString()) {
            m_target.workingDir = target["workingDir"].asString();
        }
    }

    // 验证 path 不为空
    if (m_target.path.empty()) {
        LOG_ERROR("配置中 target.path 为空");
        return false;
    }

    return true;
}

// 获取目标程序配置
const TargetConfig& ConfigManager::getTarget() const {
    return m_target;
}

// 获取配置文件路径
const std::string& ConfigManager::getFilePath() const {
    return m_filePath;
}

// 启动文件变更监听
// 监听配置文件所在目录的文件写入事件
HANDLE ConfigManager::startWatching() {
    if (m_filePath.empty()) {
        LOG_WARN("配置文件路径为空，无法启动监听");
        return nullptr;
    }

    // 获取配置文件所在目录
    fs::path configPath(m_filePath);
    fs::path dirPath = configPath.parent_path();
    if (dirPath.empty()) {
        dirPath = ".";
    }

    // FindFirstChangeNotification 监听目录
    m_hChangeNotify = FindFirstChangeNotificationW(
        dirPath.wstring().c_str(),
        FALSE,  // 不递归监视子目录
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME
    );

    if (m_hChangeNotify == INVALID_HANDLE_VALUE || m_hChangeNotify == nullptr) {
        LOG_ERROR("启动配置文件监听失败: %s", dirPath.string().c_str());
        m_hChangeNotify = nullptr;
        return nullptr;
    }

    LOG_INFO("配置文件监听已启动: %s", dirPath.string().c_str());
    return m_hChangeNotify;
}

// 停止文件变更监听
void ConfigManager::stopWatching() {
    if (m_hChangeNotify) {
        FindCloseChangeNotification(m_hChangeNotify);
        m_hChangeNotify = nullptr;
    }
}

// 检查文件变更并重新加载
// 返回 true 表示 target.path 发生了变化
bool ConfigManager::checkAndReload() {
    if (!m_hChangeNotify) {
        return false;
    }

    // 重置通知，继续监听下一次变更
    FindNextChangeNotification(m_hChangeNotify);

    // 重新读取文件
    std::ifstream file(m_filePath);
    if (!file.is_open()) {
        LOG_WARN("配置文件无法访问，保持当前配置");
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    if (content.empty()) {
        return false;
    }

    // 保存旧配置用于对比
    std::string oldPath = m_target.path;

    // 重新解析
    if (!parseJson(content)) {
        LOG_ERROR("配置文件变更后解析失败，保持当前配置");
        m_target.path = oldPath;  // 恢复旧值
        return false;
    }

    // 检查 target.path 是否变化
    if (m_target.path != oldPath) {
        LOG_INFO("配置已变更: target.path %s → %s", oldPath.c_str(), m_target.path.c_str());
        m_prevTarget.path = oldPath;
        return true;
    }

    // path 未变，但其他配置可能变了，更新 prevTarget
    m_prevTarget = m_target;
    LOG_INFO("配置文件已重载，target.path 未变化");
    return false;
}
