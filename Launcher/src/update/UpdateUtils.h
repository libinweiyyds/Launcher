#pragma once

#include <string>
#include <vector>

// 版本发布信息
struct PublishInfo {
    bool valid = false;
    int softwareId = 0;
    std::string minVersion;
    std::string recommendVersion;
    int forceUpgrade = 0;
    int clientReserveNum = 2;
    std::string remark;
};

// 远程下发的运行时配置
struct RemoteConfig {
    bool valid = false;
    std::string sha256;
    std::string configId;       // 服务端配置记录 ID
    std::string content;
    std::string localFilePath;  // 相对于管理软件 exe 目录
};

// 更新工具类：版本检查、Hash 查询、下载 URL、config 更新、版本比较
class UpdateUtils {
public:
    // 请求版本发布信息
    static PublishInfo fetchPublishInfo(const std::string& host, int port,
                                         const std::string& softwareName);

    // 查询文件远程 SHA256 Hash
    static std::string fetchFileHash(const std::string& host, int port,
                                      int softwareId, const std::string& version);

    // 构建下载 URL
    static std::string buildDownloadUrl(const std::string& host, int port,
                                         int softwareId, const std::string& version);

    // 更新 config.json 中 version 字段（不改动其他字段）
    static bool updateConfigVersion(const std::string& configPath,
                                     const std::string& newVersion,
                                     const std::vector<std::string>& history);

    // 版本号比较：支持 "v1.0.0.1" / "1.0.0.0"
    // 返回 -1(a<b), 0(==), 1(a>b)
    static int compareVersion(const std::string& a, const std::string& b);

    // 请求远程配置（按 hostName 拉取本机个性化配置）
    // GET /sms/software/getClientSoftWareConfig?hostName=...&softwareName=...&version=...
    // 每台电脑只会获取到本机自己的那份配置（避免被服务器默认值覆盖）
    static RemoteConfig fetchClientSoftwareConfig(const std::string& host, int port,
                                                  const std::string& hostName,
                                                  const std::string& softwareName,
                                                  const std::string& version);

    // 拉取默认配置（旧接口，所有客户端返回相同内容，作为首次安装兜底）
    // GET /sms/software/getConfig?softwareName=...&version=...
    static RemoteConfig fetchDefaultConfig(const std::string& host, int port,
                                           const std::string& softwareName,
                                           const std::string& version);

    // 请求远程配置（时序：先本机配置，无返回再默认配置）
    // softwareName 中文产品名
    // 返回 RemoteConfig，valid=false 表示请求/解析失败
    static RemoteConfig fetchRemoteConfig(const std::string& host, int port,
                                          const std::string& hostName,
                                          const std::string& softwareName,
                                          const std::string& version);

    // 上传本机配置
    // POST /sms/software/addClientConfig
    // jsonBody: 完整 JSON 字符串（已由调用方构造）
    // 返回 true 表示 code==200 视为成功
    static bool uploadClientConfig(const std::string& host, int port,
                                   const std::string& jsonBody);

    // 获取当前 Windows 登录用户名（UTF-8 narrow）
    // 用于 hostName 字段
    static std::string getCurrentUserName();
};
