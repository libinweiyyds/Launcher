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
bool ConfigManager::load(const std::string& filePath) {
    m_filePath = filePath;

    std::ifstream file(m_filePath);
    if (!file.is_open()) {
        LOG_WARN("配置文件不存在: %s，使用默认配置", m_filePath.c_str());
        return true;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    if (content.empty()) {
        LOG_WARN("配置文件为空: %s，使用默认配置", m_filePath.c_str());
        return true;
    }

    if (!parseJson(content)) {
        LOG_ERROR("配置文件 JSON 解析失败: %s", m_filePath.c_str());
        return false;
    }

    LOG_INFO("配置文件加载成功: %s (version=%s, software=%s)",
        m_filePath.c_str(), m_version.current.c_str(), m_software.name.c_str());
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

    // 读取 websocket 配置
    if (root.isMember("websocket") && root["websocket"].isObject()) {
        const Json::Value& ws = root["websocket"];
        if (ws.isMember("address") && ws["address"].isString()) {
            m_websocket.address = ws["address"].asString();
        }
        if (ws.isMember("port") && ws["port"].isInt()) {
            m_websocket.port = ws["port"].asInt();
        }
        if (ws.isMember("group") && ws["group"].isString()) {
            m_websocket.group = ws["group"].asString();
        }
        if (ws.isMember("id") && ws["id"].isInt()) {
            m_websocket.id = ws["id"].asInt();
        }
    }

    // 读取 version 配置
    if (root.isMember("version") && root["version"].isObject()) {
        const Json::Value& ver = root["version"];
        if (ver.isMember("current") && ver["current"].isString()) {
            m_version.current = ver["current"].asString();
        }
        if (ver.isMember("history") && ver["history"].isArray()) {
            m_version.history.clear();
            for (const auto& v : ver["history"]) {
                if (v.isString()) {
                    m_version.history.push_back(v.asString());
                }
            }
        }
    }

    // 读取 software 配置
    if (root.isMember("software") && root["software"].isObject()) {
        const Json::Value& sw = root["software"];
        if (sw.isMember("name") && sw["name"].isString()) {
            m_software.name = sw["name"].asString();
        }
        if (sw.isMember("exeName") && sw["exeName"].isString()) {
            m_software.exeName = sw["exeName"].asString();
        }
    }

    return true;
}

// 获取 WebSocket 配置
const WebsocketConfig& ConfigManager::getWebsocket() const {
    return m_websocket;
}

// 获取版本信息
const VersionInfo& ConfigManager::getVersion() const {
    return m_version;
}

// 获取软件配置
const SoftwareConfig& ConfigManager::getSoftware() const {
    return m_software;
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
bool ConfigManager::checkAndReload() {
    if (!m_hChangeNotify) return false;

    FindNextChangeNotification(m_hChangeNotify);

    std::ifstream file(m_filePath);
    if (!file.is_open()) { LOG_WARN("配置文件无法访问，保持当前配置"); return false; }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();
    if (content.empty()) return false;

    std::string oldVer = m_version.current;
    if (!parseJson(content)) {
        LOG_ERROR("配置文件变更后解析失败，保持当前配置");
        m_version.current = oldVer;
        return false;
    }

    if (m_version.current != oldVer) {
        LOG_INFO("配置已变更: version %s → %s", oldVer.c_str(), m_version.current.c_str());
        return true;
    }

    LOG_INFO("配置文件已重载，version 未变化");
    return false;
}
