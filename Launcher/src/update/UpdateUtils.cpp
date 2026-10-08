#include "UpdateUtils.h"
#include "HttpClient.h"
#include "../logger/Logger.h"
#include <Windows.h>
#undef ERROR
#ifdef max
#undef max
#endif
#include <json/json.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

// URL 编码（简单实现，对中文等非 ASCII 字符进行百分号编码）
std::string urlEncode(const std::string& s) {
    std::ostringstream escaped;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::hex << (int)c;
        }
    }
    return escaped.str();
}

// 请求版本发布信息
PublishInfo UpdateUtils::fetchPublishInfo(const std::string& host, int port,
                                           const std::string& softwareName) {
    PublishInfo info;
    std::string url = "http://" + host + ":" + std::to_string(port) +
        "/sms/software/publish/getPublishBySoftWareName?softWareName=" + urlEncode(softwareName);

    LOG_INFO("checking publish: %s (软件名: %s)", url.c_str(), softwareName.c_str());
    HttpResponse resp = HttpClient::get(url);
    if (!resp.ok || resp.body.empty()) {
        LOG_WARN("publish check failed (network)");
        return info;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(resp.body);
    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        LOG_ERROR("publish JSON parse error: %s", errors.c_str());
        return info;
    }

    int code = root.get("code", 0).asInt();
    if (code != 310) { LOG_WARN("publish code=%d, skip", code); return info; }

    const Json::Value& data = root["data"];
    if (data.empty() || data.get("status", 0).asInt() != 1) {
        LOG_INFO("software not published");
        return info;
    }

    info.valid = true;
    info.softwareId = data.get("softwareId", 0).asInt();
    info.minVersion = data.get("minVersion", "").asString();
    info.recommendVersion = data.get("recommendVersion", "").asString();
    info.forceUpgrade = data.get("forceUpgrade", 0).asInt();
    info.clientReserveNum = data.get("clientReserveNum", 2).asInt();
    info.remark = data.get("remark", "").asString();

    LOG_INFO("publish info: min=%s, rec=%s, force=%d, reserve=%d",
        info.minVersion.c_str(), info.recommendVersion.c_str(),
        info.forceUpgrade, info.clientReserveNum);
    return info;
}

// 查询文件远程 Hash
std::string UpdateUtils::fetchFileHash(const std::string& host, int port,
                                        int softwareId, const std::string& version) {
    std::string url = "http://" + host + ":" + std::to_string(port) +
        "/sms/software/getFileHash/" + std::to_string(softwareId) + "/" + version;

    LOG_INFO("fetching hash: %s", url.c_str());
    HttpResponse resp = HttpClient::get(url);
    if (!resp.ok || resp.body.empty()) { LOG_WARN("hash fetch failed"); return ""; }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(resp.body);
    if (!Json::parseFromStream(builder, stream, &root, &errors)) { return ""; }

    int code = root.get("code", 0).asInt();
    if (code != 200) { LOG_WARN("hash fetch code=%d", code); return ""; }

    std::string hash = root["data"].get("fileHash", "").asString();
    LOG_INFO("remote hash: %s", hash.c_str());
    return hash;
}

// 构建下载 URL
std::string UpdateUtils::buildDownloadUrl(const std::string& host, int port,
                                           int softwareId, const std::string& version) {
    return "http://" + host + ":" + std::to_string(port) +
        "/sms/software/downLoadBySoftWareIdWithVersion?softwareId=" +
        std::to_string(softwareId) + "&version=" + version;
}

// 更新 config.json
bool UpdateUtils::updateConfigVersion(const std::string& configPath,
                                       const std::string& newVersion,
                                       const std::vector<std::string>& history) {
    // 读取现有配置
    std::ifstream inFile(configPath);
    if (!inFile) { LOG_ERROR("cannot read config: %s", configPath.c_str()); return false; }
    std::stringstream buffer;
    buffer << inFile.rdbuf();
    inFile.close();

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(buffer.str());
    if (!Json::parseFromStream(builder, stream, &root, &errors)) { return false; }

    // 修改 version
    root["version"]["current"] = newVersion;
    Json::Value histArr(Json::arrayValue);
    for (const auto& v : history) histArr.append(v);
    root["version"]["history"] = histArr;

    // 写回（保持中文不转义、不修改 target/software/server 等字段）
    std::ofstream outFile(configPath);
    if (!outFile) { LOG_ERROR("cannot write config: %s", configPath.c_str()); return false; }
    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = "    ";
    wbuilder["emitUTF8"] = true;
    outFile << Json::writeString(wbuilder, root);
    outFile.close();

    LOG_INFO("config updated: version=%s", newVersion.c_str());
    return true;
}

// 解析 HTTP 响应为 RemoteConfig（共用解析逻辑）
static RemoteConfig parseConfigResponse(const HttpResponse& resp, const char* tag) {
    RemoteConfig cfg;
    if (!resp.ok || resp.body.empty()) {
        LOG_WARN("%s fetch failed (network)", tag);
        return cfg;
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(resp.body);
    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        LOG_ERROR("%s JSON parse error: %s", tag, errors.c_str());
        return cfg;
    }
    int code = root.get("code", 0).asInt();
    if (code != 200) {
        LOG_WARN("%s code=%d, skip", tag, code);
        return cfg;
    }
    const Json::Value& data = root["data"];
    if (data.empty()) {
        LOG_WARN("%s data empty", tag);
        return cfg;
    }
    cfg.sha256 = data.get("sha256", "").asString();
    cfg.configId = data.get("config_id", "").asString();
    cfg.content = data.get("content", "").asString();
    cfg.localFilePath = data.get("localFilePath", "").asString();
    cfg.valid = !cfg.sha256.empty() && !cfg.content.empty() && !cfg.localFilePath.empty();
    if (cfg.valid) {
        LOG_INFO("%s fetched: sha256=%s..., configId=%s, localFilePath=%s",
            tag, cfg.sha256.substr(0, 8).c_str(), cfg.configId.c_str(), cfg.localFilePath.c_str());
    } else {
        LOG_WARN("%s incomplete (sha256/content/localFilePath 任一为空)", tag);
    }
    return cfg;
}

// 请求本机配置（按 hostName 拉取，每台电脑独立）
// GET /sms/software/getClientSoftWareConfig?hostName=...&softwareName=...&version=...
RemoteConfig UpdateUtils::fetchClientSoftwareConfig(const std::string& host, int port,
                                                    const std::string& hostName,
                                                    const std::string& softwareName,
                                                    const std::string& version) {
    std::string url = "http://" + host + ":" + std::to_string(port) +
        "/sms/software/getClientSoftWareConfig?hostName=" + urlEncode(hostName) +
        "&softwareName=" + urlEncode(softwareName) +
        "&version=" + version;
    LOG_INFO("fetching client software config: %s (软件名: %s)", url.c_str(), softwareName.c_str());
    HttpResponse resp = HttpClient::get(url);
    return parseConfigResponse(resp, "client config");
}

// 拉取默认配置（旧接口，所有客户端共享，作为首次安装兜底）
// GET /sms/software/getConfig?softwareName=...&version=...
RemoteConfig UpdateUtils::fetchDefaultConfig(const std::string& host, int port,
                                             const std::string& softwareName,
                                             const std::string& version) {
    std::string url = "http://" + host + ":" + std::to_string(port) +
        "/sms/software/getConfig?softwareName=" + urlEncode(softwareName) +
        "&version=" + version;
    LOG_INFO("fetching default config: %s (软件名: %s)", url.c_str(), softwareName.c_str());
    HttpResponse resp = HttpClient::get(url);
    return parseConfigResponse(resp, "default config");
}

// 请求远程配置（时序：先本机配置，无返回再默认配置）
RemoteConfig UpdateUtils::fetchRemoteConfig(const std::string& host, int port,
                                            const std::string& hostName,
                                            const std::string& softwareName,
                                            const std::string& version) {
    // 1. 先拉本机配置（每台电脑自己的那份，避免被默认值覆盖用户修改）
    RemoteConfig cfg = fetchClientSoftwareConfig(host, port, hostName, softwareName, version);
    if (cfg.valid) return cfg;

    // 2. 本机无配置（首次安装）→ 拉默认配置
    LOG_INFO("fetchRemoteConfig: 本机配置无效，fallback 到默认配置");
    cfg = fetchDefaultConfig(host, port, softwareName, version);
    return cfg;
}

// 上传本机配置
// POST /sms/software/addClientConfig
// jsonBody: 调用方构造好的 JSON 字符串
bool UpdateUtils::uploadClientConfig(const std::string& host, int port,
                                     const std::string& jsonBody) {
    std::string url = "http://" + host + ":" + std::to_string(port) +
        "/sms/software/addClientConfig";
    LOG_INFO("uploading client config: %s", url.c_str());
    HttpResponse resp = HttpClient::post(url, jsonBody);
    if (!resp.ok || resp.body.empty()) {
        LOG_WARN("upload failed (network)");
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(resp.body);
    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        LOG_ERROR("upload response JSON parse error: %s", errors.c_str());
        return false;
    }
    int code = root.get("code", 0).asInt();
    if (code != 200 && code != 307) {
        LOG_WARN("upload code=%d, msg=%s", code, root.get("msg", "").asString().c_str());
        return false;
    }
    LOG_INFO("upload success");
    return true;
}

// 获取当前 Windows 登录用户名（UTF-8 narrow）
// 用于 hostName 字段
std::string UpdateUtils::getCurrentUserName() {
    wchar_t username[256] = { 0 };
    DWORD size = 256;
    if (!GetUserNameW(username, &size)) {
        LOG_WARN("GetUserNameW failed");
        return "";
    }
    // UTF-16 → UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, username, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return "";
    std::string result(utf8Len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, username, -1, &result[0], utf8Len, nullptr, nullptr);
    return result;
}

// 版本号比较
int UpdateUtils::compareVersion(const std::string& a, const std::string& b) {
    // 去掉前导 v/V
    auto trimV = [](const std::string& s) -> std::string {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) return s.substr(1);
        return s;
    };
    std::string va = trimV(a), vb = trimV(b);

    // 按 "." 分割
    auto split = [](const std::string& s) -> std::vector<int> {
        std::vector<int> parts;
        std::istringstream ss(s);
        std::string token;
        while (std::getline(ss, token, '.')) {
            try { parts.push_back(std::stoi(token)); }
            catch (...) { parts.push_back(0); }
        }
        return parts;
    };

    std::vector<int> pa = split(va), pb = split(vb);
    size_t maxLen = std::max(pa.size(), pb.size());
    pa.resize(maxLen, 0);
    pb.resize(maxLen, 0);

    for (size_t i = 0; i < maxLen; ++i) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return 1;
    }
    return 0;
}
