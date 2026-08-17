#pragma once

#include <string>
#include <vector>

// 版本管理：版本比较、切换、历史记录、清理
class VersionManager {
public:
    // 设置当前版本信息
    void setCurrent(const std::string& version);
    // 设置版本历史
    void setHistory(const std::vector<std::string>& history);

    // 获取当前版本
    const std::string& getCurrent() const;
    // 获取版本历史
    const std::vector<std::string>& getHistory() const;

    // 是否需要更新
    bool needUpdate(const std::string& targetVersion) const;

    // 记录新版本（插入到历史最前面）
    void recordVersion(const std::string& version);

    // 保留最近 keepCount 个版本，删除旧目录
    void cleanup(const std::string& versionsDir, int keepCount = 2);

    // 剔除 history 中所有版本号 > current 的项
    // 保证不变式：history 中所有版本 ≤ current
    void pruneHistoryAboveCurrent();

    // 查找目录中的 start.exe（或第一个 exe）
    static std::string findExeInDir(const std::string& dir);

private:
    std::string m_current;
    std::vector<std::string> m_history;
};
