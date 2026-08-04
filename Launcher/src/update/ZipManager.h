#pragma once

#include <string>

// ZIP 解压管理器：基于 minizip 解压 zip 文件到指定目录
class ZipManager {
public:
    // 解压 zip 文件到目标目录
    // zipPath: zip 文件完整路径
    // destDir: 解压目标目录（自动创建）
    // 返回 true 表示解压成功
    bool extract(const std::string& zipPath, const std::string& destDir);
};
