#pragma once

#include <string>
#include <vector>
#include <functional>
#include <json/json.h>
#include "UpdateUtils.h"

// 远程配置同步工具：把服务端下发的配置写入管理软件 exe 的本地路径下
class ConfigSync {
public:
    // 把 remote.content 写入 baseDir/localFilePath
    // 如果本地文件已存在且 sha256 与 remote.sha256 一致 → 跳过（无需重写）
    // 否则写入新内容，写完再次校验 sha256
    // baseDir: 管理软件 exe 所在目录（绝对路径）
    // 返回 true 表示成功或跳过一致；false 表示写入/校验失败
    static bool syncConfig(const RemoteConfig& remote, const std::wstring& baseDir);

    // 版本升级配置继承 + 合并（首次启动新版本时调用）
    // 行为：
    //   1. baseDir/{configDir} 已存在 → 不做任何事，返回 true（走 hash 校验）
    //   2. baseDir/{configDir} 不存在：
    //      a. 遍历 history 找第一个有 config 目录的旧版本
    //      b. 找到 → copyDirRecursive 复制 + JSON 合并（用户值优先）
    //      c. 找不到 → 当作全新安装，直接写入 remote.content，调用 onFirstInstall 回调
    // 返回 true 表示处理完成
    // onFirstInstall（可选）：首次安装时回调（参数：写入本地后的 sha256），调用方决定是否写 software.sha256
    static bool inheritAndMergeConfig(const RemoteConfig& remote,
                                      const std::wstring& baseDir,
                                      const std::vector<std::string>& history,
                                      const std::wstring& versionsRoot,
                                      const std::string& currentVersion,
                                      const std::function<void(const std::string&)>& onFirstInstall);

    // 退出时调用：计算 baseDir/{configRelPath} 的 sha256，与 baselineSha256 比对
    // 不同 → POST 上传当前内容；上传成功 → 调用 onUploaded 回调更新 sha256
    // 返回 true 表示已上传成功（或未变化无需上传）
    // 上传失败 → 返回 false（自动重试机制）
    static bool uploadConfigIfChanged(const std::string& host, int port,
                                      const std::string& configId,
                                      const std::string& hostName,
                                      const std::string& baseDir,
                                      const std::string& configRelPath,
                                      const std::string& baselineSha256,
                                      const std::function<void(const std::string&)>& onUploaded);

    // 工具函数：递归复制目录
    static void copyDirRecursive(const std::wstring& src, const std::wstring& dst);

    // 工具函数：JSON 深度合并（旧值优先、新增字段追加）
    static Json::Value deepMergeJson(const Json::Value& oldV, const Json::Value& newV);
};