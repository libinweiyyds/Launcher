#include "Logger.h"

#include <iostream>
#include <filesystem>
#include <cstdio>
#include <ctime>

namespace fs = std::filesystem;

// 获取全局唯一的 Logger 实例（线程安全的懒加载单例）
Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

// 析构函数：确保文件正确关闭
Logger::~Logger() {
    shutdown();
}

// 初始化日志系统
// 创建日志目录，打开当天日期的日志文件
bool Logger::init(const std::string& logDir) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    m_logDir = logDir;

    // 创建日志目录，如果不存在
    std::error_code ec;
    fs::create_directories(m_logDir, ec);
    if (ec) {
        std::cerr << "日志目录创建失败: " << ec.message() << "，使用当前目录" << std::endl;
        m_logDir = ".";
    }

    // 打开当天日期的日志文件
    openTodayLog();
    if (!m_file.is_open()) {
        std::cerr << "无法打开日志文件" << std::endl;
        return false;
    }

    m_initialized = true;
    // 写入启动标记，便于区分每次运行的日志
    write(LogLevel::INFO, "SYSTEM", "========== Launcher 启动 ==========");

    return true;
}

// 关闭日志系统
void Logger::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_initialized && m_file.is_open()) {
        write(LogLevel::INFO, "SYSTEM", "========== Launcher 退出 ==========");
        m_file.flush();
        m_file.close();
        m_initialized = false;
    }
}

// 动态调整日志级别
void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_level = level;
    if (m_file.is_open()) {
        m_file << timestamp() << " [INFO] [SYSTEM] 日志级别已切换为: " << levelName(level) << std::endl;
    }
}

// 核心写日志方法
// 写入前检查是否跨天，跨天则自动切换到新文件
void Logger::write(LogLevel level, const char* tag, const std::string& message) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // 级别过滤
    if (level < m_level) return;

    // 未初始化时的兜底输出
    if (!m_file.is_open()) {
        std::cerr << timestamp() << " [" << levelName(level) << "] [" << tag << "] " << message << std::endl;
        return;
    }

    // 检查是否跨天，需要切换日志文件
    std::string today = todayLogFile();
    if (today != m_currentDate) {
        m_file.flush();
        m_file.close();
        openTodayLog();
        if (m_file.is_open()) {
            m_file << timestamp() << " [INFO] [SYSTEM] 日期变更，日志已切换到: " << today << std::endl;
        }
    }

    // 写入格式化日志行
    if (m_file.is_open()) {
        m_file << timestamp() << " [" << levelName(level) << "] [" << tag << "] " << message << std::endl;
    }
}

// 生成当前时间戳，精确到毫秒
// 格式：[2026-07-22 12:00:00.123]
std::string Logger::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    tm localTime;
    localtime_s(&localTime, &time_t);

    char buf[64];
    snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03lld]",
        1900 + localTime.tm_year,
        localTime.tm_mon + 1,
        localTime.tm_mday,
        localTime.tm_hour,
        localTime.tm_min,
        localTime.tm_sec,
        static_cast<long long>(ms.count()));

    return buf;
}

// 获取日志级别的名称
const char* Logger::levelName(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        default:              return "UNKN ";
    }
}

// 生成当天日期对应的日志文件名
// 格式：2026-7-22.log
std::string Logger::todayLogFile() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    tm localTime;
    localtime_s(&localTime, &time_t);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d-%d-%d.log",
        1900 + localTime.tm_year,
        localTime.tm_mon + 1,
        localTime.tm_mday);

    return buf;
}

// 打开（或切换到）当天日期的日志文件
void Logger::openTodayLog() {
    m_currentDate = todayLogFile();
    std::string filePath = m_logDir + "/" + m_currentDate;
    m_file.open(filePath, std::ios::out | std::ios::app);
}
