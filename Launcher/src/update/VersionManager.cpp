#include "VersionManager.h"
#include "../logger/Logger.h"
#include "UpdateUtils.h"
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

// 内部：剔除 history 中所有版本号 > current 的项
// 用于保证不变式 "history 中所有版本 ≤ current"
static int compareVersionSafe(const std::string& a, const std::string& b) {
    return UpdateUtils::compareVersion(a, b);
}

void VersionManager::setCurrent(const std::string& version) {
    m_current = version;
    // 同步清理 history 中比 current 还新的项
    pruneHistoryAboveCurrent();
}

void VersionManager::setHistory(const std::vector<std::string>& history) {
    m_history = history;
    pruneHistoryAboveCurrent();
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
// 变更：把 m_current 设为新版本后，剔除 history 中 > 新版本的项
void VersionManager::recordVersion(const std::string& version) {
    // 移除已存在的同版本记录（如果有）
    m_history.erase(
        std::remove(m_history.begin(), m_history.end(), version),
        m_history.end()
    );
    // 插入到最前面
    m_history.insert(m_history.begin(), version);
    m_current = version;
    // 保证 history ≤ current
    pruneHistoryAboveCurrent();
}

// 剔除 history 中所有版本号 > current 的项
void VersionManager::pruneHistoryAboveCurrent() {
    if (m_current.empty()) return;
    auto removed = std::remove_if(m_history.begin(), m_history.end(),
        [this](const std::string& v) {
            return compareVersionSafe(v, m_current) > 0;
        });
    if (removed != m_history.end()) {
        for (auto it = removed; it != m_history.end(); ++it) {
            LOG_WARN("剔除 history 中超过 current (%s) 的版本: %s",
                m_current.c_str(), it->c_str());
        }
        m_history.erase(removed, m_history.end());
    }
}

// 清理旧版本目录，保留最近 keepCount 个
// 同步维护 m_history 字段，确保 config 与磁盘目录一致
// 当前版本目录永不删除
void VersionManager::cleanup(const std::string& versionsDir, int keepCount) {
    // 先保证 history ≤ current 不变式
    pruneHistoryAboveCurrent();

    if (m_history.size() <= static_cast<size_t>(keepCount)) {
        return;
    }

    // 收集要保留的版本（保留前 keepCount 个 + 当前版本）
    std::vector<std::string> newHist;
    for (size_t i = 0; i < m_history.size() && newHist.size() < static_cast<size_t>(keepCount); ++i) {
        newHist.push_back(m_history[i]);
    }
    // 确保当前版本在 history 里
    if (!m_current.empty() &&
        std::find(newHist.begin(), newHist.end(), m_current) == newHist.end()) {
        newHist.insert(newHist.begin(), m_current);
    }

    // 删除超出保留范围的目录（当前版本目录保护）
    for (const auto& ver : m_history) {
        if (ver == m_current) continue;  // 永不删除当前版本
        if (std::find(newHist.begin(), newHist.end(), ver) != newHist.end()) continue;
        fs::path dir = fs::path(versionsDir) / ver;
        std::error_code ec;
        if (fs::exists(dir)) {
            fs::remove_all(dir, ec);
            if (!ec) {
                LOG_INFO("已清理旧版本: %s", ver.c_str());
            }
        }
    }

    // 同步 history 字段
    m_history = newHist;
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
