#pragma once

#include <string>

// HTTP 下载管理器：WinHTTP 实现，支持下载进度跟踪
class DownloadManager {
public:
    // 下载文件到 .tmp 路径
    // url: 下载地址
    // tmpPath: 临时文件路径（.tmp 后缀）
    // 返回 true 表示下载成功
    bool download(const std::string& url, const std::string& tmpPath);

    // 计算文件 SHA256 哈希
    static std::string sha256(const std::string& filePath);

    // 取消当前下载
    void cancel();

private:
    bool m_cancelled = false;
};
