#pragma once

// 消除 Windows 头文件中可能冲突的宏定义
#ifdef ERROR
#undef ERROR
#endif
#ifdef DEBUG
#undef DEBUG
#endif

#include <string>
#include <mutex>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>

// 日志级别定义：数值越小越详细
enum class LogLevel {
    TRACE = 0,  // 最详细的跟踪信息
    DEBUG = 1,  // 调试信息
    INFO  = 2,  // 常规运行信息
    WARN  = 3,  // 警告信息
    ERROR = 4   // 错误信息
};

// 日志单例类：提供线程安全的文件日志功能
// 按日期命名日志文件（如 2026-7-22.log），跨天自动切换
class Logger {
public:
    // 获取全局唯一实例
    static Logger& instance();

    // 初始化日志系统
    // logDir: 日志文件存放目录
    bool init(const std::string& logDir);

    // 关闭日志系统，刷新缓冲区并关闭文件
    void shutdown();

    // 动态设置日志级别，低于此级别的日志将被忽略
    void setLevel(LogLevel level);

    // 写入一条日志记录（内部使用，推荐通过宏调用）
    void write(LogLevel level, const char* tag, const std::string& message);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 生成时间戳字符串，格式：[2026-07-21 12:00:00.123]
    static std::string timestamp();

    // 获取日志级别对应的名称
    static const char* levelName(LogLevel level);

    // 生成当天日期对应的日志文件名（如 "2026-7-22.log"）
    static std::string todayLogFile();

    // 打开（或切换到）当天的日志文件
    void openTodayLog();

    std::recursive_mutex m_mutex;   // 递归互斥锁，允许同一线程多次锁定
    std::ofstream m_file;           // 日志输出文件流
    std::string m_logDir;           // 日志目录路径
    std::string m_currentDate;      // 当前日志文件对应的日期（用于跨天检测）
    LogLevel m_level = LogLevel::INFO;  // 当前日志级别，默认 INFO
    bool m_initialized = false;     // 是否已完成初始化
};

// 便捷日志宏
// 使用 do-while 封装，内部用局部缓冲区格式化字符串
// 参数直接在使用处展开，无需捕获，支持访问调用方的成员变量和局部变量

#define LOG_TRACE(fmt, ...) \
    do { \
        char _log_buf[1024]; \
        snprintf(_log_buf, sizeof(_log_buf), fmt, ##__VA_ARGS__); \
        Logger::instance().write(LogLevel::TRACE, "APP", _log_buf); \
    } while(0)

#define LOG_DEBUG(fmt, ...) \
    do { \
        char _log_buf[1024]; \
        snprintf(_log_buf, sizeof(_log_buf), fmt, ##__VA_ARGS__); \
        Logger::instance().write(LogLevel::DEBUG, "APP", _log_buf); \
    } while(0)

#define LOG_INFO(fmt, ...) \
    do { \
        char _log_buf[1024]; \
        snprintf(_log_buf, sizeof(_log_buf), fmt, ##__VA_ARGS__); \
        Logger::instance().write(LogLevel::INFO, "APP", _log_buf); \
    } while(0)

#define LOG_WARN(fmt, ...) \
    do { \
        char _log_buf[1024]; \
        snprintf(_log_buf, sizeof(_log_buf), fmt, ##__VA_ARGS__); \
        Logger::instance().write(LogLevel::WARN, "APP", _log_buf); \
    } while(0)

#define LOG_ERROR(fmt, ...) \
    do { \
        char _log_buf[1024]; \
        snprintf(_log_buf, sizeof(_log_buf), fmt, ##__VA_ARGS__); \
        Logger::instance().write(LogLevel::ERROR, "APP", _log_buf); \
    } while(0)

// 带自定义标签的日志宏
#define LOG_TAG(tag, level, fmt, ...) \
    do { \
        char _log_buf[1024]; \
        snprintf(_log_buf, sizeof(_log_buf), fmt, ##__VA_ARGS__); \
        Logger::instance().write(level, tag, _log_buf); \
    } while(0)
