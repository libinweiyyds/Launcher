#pragma once

#include <string>
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
};