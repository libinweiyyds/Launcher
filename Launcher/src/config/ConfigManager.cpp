#include "ConfigManager.h"
#include "../logger/Logger.h"
#include <json/json.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <rpc.h>

#pragma comment(lib, "rpcrt4.lib")

namespace fs = std::filesystem;

// 生成一个新的 UUID（36 字符标准格式 "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"）
static std::string generateUuid() {
    UUID uuid;
    UuidCreate(&uuid);
    unsigned char* str = nullptr;
    if (UuidToStringA(&uuid, &str) != RPC_S_OK || str == nullptr) {
        return "";
    }
    std::string result(reinterpret_cast<const char*>(str));
    RpcStringFreeA(&str);
    return result;
}

ConfigManager::ConfigManager() = default;

ConfigManager::~ConfigManager() {
    stopWatching();
}

// 加载配置文件
bool ConfigManager::load(const std::string& filePath) {
    m_filePath = filePath;

    // 文件不存在 → 直接落盘默认配置（含默认服务器地址 localhost:8080）
    std::ifstream file(m_filePath);
    if (!file.is_open()) {
        LOG_WARN("配置文件不存在: %s，写入默认配置（含默认服务器 localhost:8080）",
            m_filePath.c_str());
        return writeConfigToDisk(/*createIfMissing=*/true);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    if (content.empty()) {
        LOG_WARN("配置文件为空: %s，写入默认配置", m_filePath.c_str());
        return writeConfigToDisk(/*createIfMissing=*/true);
    }

    if (!parseJson(content)) {
        LOG_ERROR("配置文件 JSON 解析失败: %s", m_filePath.c_str());
        return false;
    }

    // 已存在但缺 server 段 → 补全默认服务器地址并落盘
    // 启动时要拿到真实的服务器地址/端口，避免每次都要手改 config.json
    Json::Value probe;
    Json::CharReaderBuilder b;
    std::string err;
    std::istringstream ss(content);
    bool parsed = Json::parseFromStream(b, ss, &probe, &err);
    if (parsed && !probe.isMember("server")) {
        LOG_WARN("配置文件缺少 server 段，补全默认 localhost:8080 并写盘");
        if (!writeConfigToDisk(/*createIfMissing=*/false)) {
            LOG_ERROR("补全 server 段写盘失败");
        }
    }

    LOG_INFO("配置文件加载成功: %s (version=%s, software=%s, server=%s:%d)",
        m_filePath.c_str(), m_version.current.c_str(), m_software.softwareName.c_str(),
        m_websocket.address.c_str(), m_websocket.port);
    return true;
}

// 把当前内存配置写到磁盘
// createIfMissing=true: 文件不存在则创建；包含全部默认段（version / server / software）
// createIfMissing=false: 文件已存在，仅补全缺失字段（目前只补 server 段），保留用户已写入的内容
bool ConfigManager::writeConfigToDisk(bool createIfMissing) {
    if (m_filePath.empty()) {
        LOG_ERROR("配置文件路径为空，无法写盘");
        return false;
    }

    Json::Value root;

    if (!createIfMissing) {
        // 文件已存在，读出原内容作为基线（仅补缺失字段，不覆盖已有字段）
        std::ifstream inFile(m_filePath);
        if (inFile.is_open()) {
            std::stringstream buf;
            buf << inFile.rdbuf();
            inFile.close();
            Json::CharReaderBuilder b;
            std::string err;
            std::istringstream ss(buf.str());
            if (Json::parseFromStream(b, ss, &root, &err) && root.isObject()) {
                // 已解析为 root 继续
            } else {
                root = Json::Value(Json::objectValue);
            }
        }
    }

    // 补全 server 段（HTTP API 与 WebSocket 共用同一服务器地址/端口）
    if (!root.isMember("server")) {
        Json::Value sv(Json::objectValue);
        sv["address"] = kDefaultAddress;
        sv["port"] = kDefaultPort;
        sv["group"] = m_websocket.group.empty() ? "launcher" : m_websocket.group;
        // id 缺失时生成新 UUID，保证每台设备唯一
        if (m_websocket.id.empty()) {
            m_websocket.id = generateUuid();
            LOG_INFO("已生成新设备 UUID: %s", m_websocket.id.c_str());
        }
        sv["id"] = m_websocket.id;
        root["server"] = sv;
        LOG_INFO("已补全默认服务器配置: %s:%d", kDefaultAddress, kDefaultPort);
    }

    // 创建模式下写入完整默认骨架
    if (createIfMissing) {
        if (!root.isMember("version")) {
            Json::Value ver(Json::objectValue);
            ver["current"] = m_version.current.empty() ? "1.0.0.0" : m_version.current;
            Json::Value hist(Json::arrayValue);
            for (const auto& v : m_version.history) hist.append(v);
            ver["history"] = hist;
            root["version"] = ver;
        }
        if (!root.isMember("software")) {
            Json::Value sw(Json::objectValue);
            sw["softwareName"] = m_software.softwareName;
            sw["exeName"] = m_software.exeName.empty() ? "start.exe" : m_software.exeName;
            root["software"] = sw;
        }
    }

    // 确保父目录存在
    fs::path cfgPath(m_filePath);
    fs::path parentDir = cfgPath.parent_path();
    if (!parentDir.empty() && !fs::exists(parentDir)) {
        std::error_code ec;
        fs::create_directories(parentDir, ec);
        if (ec) {
            LOG_ERROR("创建配置父目录失败: %s", ec.message().c_str());
            return false;
        }
    }

    // 写盘（中文不转义；与 UpdateUtils::updateConfigVersion 保持一致）
    std::ofstream outFile(m_filePath, std::ios::trunc);
    if (!outFile.is_open()) {
        LOG_ERROR("配置文件写盘失败: %s", m_filePath.c_str());
        return false;
    }
    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = "    ";
    wbuilder["emitUTF8"] = true;
    outFile << Json::writeString(wbuilder, root);
    outFile.close();

    LOG_INFO("配置文件已写盘: %s", m_filePath.c_str());
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

    // 读取 server 配置（HTTP API 与 WebSocket 共用同一服务器地址/端口）
    if (root.isMember("server") && root["server"].isObject()) {
        const Json::Value& sv = root["server"];
        if (sv.isMember("address") && sv["address"].isString()) {
            m_websocket.address = sv["address"].asString();
        }
        if (sv.isMember("port") && sv["port"].isInt()) {
            m_websocket.port = sv["port"].asInt();
        }
        if (sv.isMember("group") && sv["group"].isString()) {
            m_websocket.group = sv["group"].asString();
        }
        // id 兼容两种类型：旧配置可能是 int，新配置是 UUID 字符串
        if (sv.isMember("id")) {
            if (sv["id"].isString()) {
                m_websocket.id = sv["id"].asString();
            } else if (sv["id"].isInt()) {
                m_websocket.id = std::to_string(sv["id"].asInt());
            }
        }
    }

    // id 空或为占位全零 → 视为未初始化，重新生成 UUID 并写盘
    auto isPlaceholderUuid = [](const std::string& s) {
        if (s.empty()) return true;
        std::string norm;
        norm.reserve(s.size());
        for (char c : s) if (c != '-') norm.push_back(c);
        if (norm.size() != 32) return false;
        for (char c : norm) if (c != '0') return false;
        return true;
    };
    if (isPlaceholderUuid(m_websocket.id)) {
        m_websocket.id = generateUuid();
        LOG_WARN("原 id 为空或占位符，已重新生成 UUID: %s", m_websocket.id.c_str());
        // 写盘让新 id 落地，下次启动保持稳定
        writeConfigToDisk(/*createIfMissing=*/false);
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
        if (sw.isMember("softwareName") && sw["softwareName"].isString()) {
            m_software.softwareName = sw["softwareName"].asString();
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

    // 热加载时如果磁盘文件缺 server 段，同步补全
    Json::Value probe;
    Json::CharReaderBuilder b;
    std::string err;
    std::istringstream ss(content);
    if (Json::parseFromStream(b, ss, &probe, &err) && probe.isObject()
        && !probe.isMember("server")) {
        LOG_WARN("热加载发现配置缺 server 段，补全默认并写盘");
        writeConfigToDisk(/*createIfMissing=*/false);
    }

    LOG_INFO("配置文件已重载，version 未变化");
    return false;
}
