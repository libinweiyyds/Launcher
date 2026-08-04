#include "ZipManager.h"
#include "../logger/Logger.h"
#include <minizip/unzip.h>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

bool ZipManager::extract(const std::string& zipPath, const std::string& destDir) {
    // 打开 zip 文件
    unzFile zip = unzOpen(zipPath.c_str());
    if (!zip) {
        LOG_ERROR("无法打开 zip 文件: %s", zipPath.c_str());
        return false;
    }

    // 获取 zip 内文件信息
    unz_global_info globalInfo;
    if (unzGetGlobalInfo(zip, &globalInfo) != UNZ_OK) {
        LOG_ERROR("无法读取 zip 文件信息");
        unzClose(zip);
        return false;
    }

    LOG_INFO("开始解压: %s → %s (%ld 个文件)", zipPath.c_str(), destDir.c_str(), globalInfo.number_entry);

    // 创建目标目录
    std::error_code ec;
    fs::create_directories(destDir, ec);

    // 遍历并解压每个文件
    for (uLong i = 0; i < globalInfo.number_entry; ++i) {
        // 获取当前文件信息
        char filenameInZip[256];
        unz_file_info fileInfo;
        if (unzGetCurrentFileInfo(zip, &fileInfo, filenameInZip, sizeof(filenameInZip), nullptr, 0, nullptr, 0) != UNZ_OK) {
            LOG_ERROR("无法获取 zip 内第 %lu 个文件的信息", i);
            unzClose(zip);
            return false;
        }

        // 构建输出路径
        fs::path outPath = fs::path(destDir) / filenameInZip;

        // 如果是目录（以 / 结尾），创建目录
        std::string filenameStr(filenameInZip);
        if (fileInfo.uncompressed_size == 0 && filenameStr.back() == '/') {
            fs::create_directories(outPath, ec);
        } else {
            // 确保父目录存在
            fs::create_directories(outPath.parent_path(), ec);

            // 打开当前文件
            if (unzOpenCurrentFile(zip) != UNZ_OK) {
                LOG_ERROR("无法打开 zip 内文件: %s", filenameInZip);
                unzClose(zip);
                return false;
            }

            // 读取并写入
            std::vector<char> buffer(fileInfo.uncompressed_size);
            int bytesRead = unzReadCurrentFile(zip, buffer.data(), static_cast<unsigned>(buffer.size()));
            if (bytesRead < 0) {
                LOG_ERROR("读取 zip 内文件失败: %s", filenameInZip);
                unzCloseCurrentFile(zip);
                unzClose(zip);
                return false;
            }

            std::ofstream outFile(outPath, std::ios::binary);
            if (!outFile) {
                LOG_ERROR("无法创建输出文件: %s", outPath.string().c_str());
                unzCloseCurrentFile(zip);
                unzClose(zip);
                return false;
            }
            outFile.write(buffer.data(), bytesRead);
            outFile.close();

            unzCloseCurrentFile(zip);
        }

        // 移动到下一个文件
        if (i < globalInfo.number_entry - 1) {
            if (unzGoToNextFile(zip) != UNZ_OK) {
                LOG_ERROR("移动到下一个 zip 条目失败");
                unzClose(zip);
                return false;
            }
        }
    }

    unzClose(zip);
    LOG_INFO("解压完成: %d 个文件 → %s", globalInfo.number_entry, destDir.c_str());
    return true;
}
