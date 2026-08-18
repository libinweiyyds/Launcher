#include "ConfigSync.h"
#include "../logger/Logger.h"
#include "DownloadManager.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

bool ConfigSync::syncConfig(const RemoteConfig& remote, const std::wstring& baseDir) {
    if (!remote.valid) {
        LOG_ERROR("ConfigSync: remote config invalid");
        return false;
    }

    // 解析目标路径：baseDir（管理软件 exe 所在目录）+ localFilePath（相对路径）
    std::string relPath = remote.localFilePath;
    // 转 wstring 给 std::filesystem（路径含中文不会乱码）
    std::wstring wideRel(relPath.begin(), relPath.end());
    fs::path target = fs::path(baseDir) / wideRel;
    std::error_code ec;

    // 父目录不存在 → 递归创建
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        LOG_ERROR("ConfigSync: cannot create parent dir: %s (%s)",
            target.parent_path().string().c_str(), ec.message().c_str());
        return false;
    }

    // 文件已存在且 hash 一致 → 跳过
    if (fs::exists(target)) {
        std::string localHash = DownloadManager::sha256(target.string());
        if (localHash == remote.sha256) {
            LOG_INFO("ConfigSync: local file up-to-date, skip: %s", target.string().c_str());
            return true;
        }
        LOG_INFO("ConfigSync: local file hash mismatch, will overwrite: %s", target.string().c_str());
    }

    // 写入新内容（二进制方式，原样保留 UTF-8 字节，避免 C++ 文本模式改换行符）
    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            LOG_ERROR("ConfigSync: cannot write file: %s", target.string().c_str());
            return false;
        }
        out.write(remote.content.data(), static_cast<std::streamsize>(remote.content.size()));
        out.close();
    }

    // 写完再次校验 sha256
    std::string newHash = DownloadManager::sha256(target.string());
    if (newHash != remote.sha256) {
        LOG_ERROR("ConfigSync: sha256 verify failed after write: exp=%s, got=%s",
            remote.sha256.c_str(), newHash.c_str());
        std::filesystem::remove(target, ec);
        return false;
    }

    LOG_INFO("ConfigSync: wrote %llu bytes to %s (sha256 OK)",
        static_cast<unsigned long long>(remote.content.size()), target.string().c_str());
    return true;
}