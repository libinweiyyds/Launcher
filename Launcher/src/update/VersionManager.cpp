#include "VersionManager.h"
#include "../logger/Logger.h"
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

void VersionManager::setCurrent(const std::string& version) {
    m_current = version;
}

void VersionManager::setHistory(const std::vector<std::string>& history) {
    m_history = history;
}

const std::string& VersionManager::getCurrent() const {
    return m_current;
}

const std::vector<std::string>& VersionManager::getHistory() const {
    return m_history;
}

// 版本号不同就需要更新
bool VersionManager::needUpdate(const std::string& targetVersion) const {
    return m_current != targetVersion;
}

// 记录新版本到历史最前面
void VersionManager::recordVersion(const std::string& version) {
    // 移除已存在的同版本记录（如果有）
    m_history.erase(
        std::remove(m_history.begin(), m_history.end(), version),
        m_history.end()
    );
    // 插入到最前面
    m_history.insert(m_history.begin(), version);
    m_current = version;
}

// 清理旧版本目录，保留最近 keepCount 个
void VersionManager::cleanup(const std::string& versionsDir, int keepCount) {
    if (m_history.size() <= static_cast<size_t>(keepCount)) {
        return;
    }

    // 保留前 keepCount 个，删除其余
    for (size_t i = keepCount; i < m_history.size(); ++i) {
        fs::path dir = fs::path(versionsDir) / m_history[i];
        std::error_code ec;
        if (fs::exists(dir)) {
            fs::remove_all(dir, ec);
            if (!ec) {
                LOG_INFO("已清理旧版本: %s", m_history[i].c_str());
            }
        }
    }

    // 裁剪历史列表
    m_history.resize(keepCount);
}

// 递归查找目录中的 exe 文件（优先 start.exe）
std::string VersionManager::findExeInDir(const std::string& dir) {
    fs::path baseDir(dir);
    if (!fs::exists(baseDir)) return "";

    // 优先查找根目录下的 start.exe
    fs::path startExe = baseDir / "start.exe";
    if (fs::exists(startExe)) return startExe.string();

    // 递归查找所有 .exe（处理 zip 内有子文件夹的情况）
    for (const auto& entry : fs::recursive_directory_iterator(baseDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".exe") {
            return entry.path().string();
        }
    }

    return "";
}
