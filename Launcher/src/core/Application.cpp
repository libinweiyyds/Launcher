#include "Application.h"
#include "../logger/Logger.h"
#include "../config/ConfigManager.h"
#include "../process/ProcessManager.h"
#include <filesystem>
#include <shellapi.h>

namespace fs = std::filesystem;

// 静态成员初始化
Application* Application::s_instance = nullptr;

// 获取全局唯一实例
Application& Application::instance() {
    static Application inst;
    return inst;
}

// 析构：确保资源释放
Application::~Application() {
    shutdown();
}

// 程序入口
// 执行初始化 → 主循环 → 清理的完整生命周期
int Application::run(HINSTANCE hInstance) {
    s_instance = this;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    if (!init()) {
        LOG_ERROR("初始化失败，程序即将退出");
        shutdown();
        return 1;
    }

    LOG_INFO("Launcher 启动完成（PID: %lu）", GetCurrentProcessId());

    // 进入主循环
    mainLoop();

    // 执行清理流程
    shutdown();
    return 0;
}

// 初始化阶段
// Logger → 单实例检查 → 加载配置 → 解析目标路径 → 启动子进程
bool Application::init() {
    // 1. 初始化日志系统
    if (!Logger::instance().init("./logs")) {
        return false;
    }

    LOG_INFO("日志系统初始化完成");

    // 2. 单实例检查
    m_hMutex = CreateMutexW(nullptr, TRUE, L"Global\\Launcher_SingleInstance");
    if (m_hMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_ERROR("检测到已有 Launcher 实例在运行，本次启动取消");
        if (m_hMutex) {
            CloseHandle(m_hMutex);
            m_hMutex = nullptr;
        }
        return false;
    }

    LOG_INFO("单实例检查通过");

    // 3. 创建模块
    m_configMgr = std::make_unique<ConfigManager>();
    m_processMgr = std::make_unique<ProcessManager>();

    // 4. 加载配置文件
    std::wstring configPath = findConfigPath();
    if (!configPath.empty()) {
        // 将宽字符路径转为 UTF-8 字符串
        int len = WideCharToMultiByte(CP_UTF8, 0, configPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string configPathStr(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, configPath.c_str(), -1, &configPathStr[0], len, nullptr, nullptr);
        m_configMgr->load(configPathStr);
    } else {
        LOG_WARN("未找到 Config/config.json，使用默认配置");
        m_configMgr->load("");  // 使用默认配置
    }

    // 5. 解析目标程序路径
    const TargetConfig& target = m_configMgr->getTarget();
    m_currentTargetPath = resolveTargetPath(target.path);

    // 检查目标文件是否存在
    if (!fs::exists(m_currentTargetPath)) {
        LOG_ERROR("目标程序不存在: %ls", m_currentTargetPath.c_str());
        return false;
    }

    // 6. 启动子进程
    std::wstring workingDir;
    if (!target.workingDir.empty()) {
        workingDir = std::wstring(target.workingDir.begin(), target.workingDir.end());
    }

    if (!m_processMgr->start(m_currentTargetPath, workingDir)) {
        LOG_ERROR("目标程序启动失败");
        return false;
    }

    m_running = true;
    return true;
}

// 主循环：同时等待子进程退出和配置文件变更
void Application::mainLoop() {
    LOG_INFO("进入主循环");

    // 获取监听句柄
    HANDLE hProcess = m_processMgr->getProcessHandle();
    HANDLE hConfig = m_configMgr->startWatching();

    while (m_running) {
        // 构建句柄数组
        HANDLE handles[2];
        DWORD count = 0;

        handles[count++] = hProcess;  // 索引 0：子进程句柄

        if (hConfig != nullptr) {
            handles[count++] = hConfig;  // 索引 1：配置文件变更通知
        }

        // 等待任意事件发生
        DWORD result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);

        if (result == WAIT_OBJECT_0) {
            // 子进程退出
            DWORD exitCode = 0;
            if (GetExitCodeProcess(hProcess, &exitCode)) {
                LOG_INFO("目标程序已退出（退出码: %lu），Launcher 将退出", exitCode);
            } else {
                LOG_INFO("目标程序已退出，Launcher 将退出");
            }
            m_running = false;
        } else if (result == WAIT_OBJECT_0 + 1) {
            // 配置文件变更 → 热加载
            LOG_INFO("检测到配置文件变更");
            if (m_configMgr->checkAndReload()) {

                // 获取新配置的目标路径
                const TargetConfig& target = m_configMgr->getTarget();
                std::wstring newPath = resolveTargetPath(target.path);

                // 检查目标路径是否变化
                if (newPath != m_currentTargetPath) {
                    LOG_INFO("目标程序路径已变更: %ls → %ls",
                        m_currentTargetPath.c_str(), newPath.c_str());

                    // 检查新目标是否存在
                    if (!fs::exists(newPath)) {
                        LOG_ERROR("新目标程序不存在: %ls，保持当前进程", newPath.c_str());
                        continue;
                    }

                    // 停止当前子进程
                    m_processMgr->stop();

                    // 启动新子进程
                    std::wstring workingDir;
                    if (!target.workingDir.empty()) {
                        workingDir = std::wstring(target.workingDir.begin(),
                            target.workingDir.end());
                    }

                    if (m_processMgr->start(newPath, workingDir)) {
                        m_currentTargetPath = newPath;
                        hProcess = m_processMgr->getProcessHandle();  // 更新句柄
                        LOG_INFO("热加载完成，新目标程序已启动");
                    } else {
                        LOG_ERROR("热加载失败：新目标程序启动失败，Launcher 将退出");
                        m_running = false;
                    }
                } else {
                    LOG_INFO("目标程序路径未变化，无需重启");
                }
            }
        } else {
            // WAIT_FAILED 或其他错误
            DWORD err = GetLastError();
            LOG_ERROR("WaitForMultipleObjects 失败（错误码: %lu）", err);
            m_running = false;
        }
    }

    m_configMgr->stopWatching();
    LOG_INFO("主循环已退出");
}

// 清理阶段：逆序释放资源
void Application::shutdown() {
    m_running = false;

    // 1. 停止配置文件监听
    if (m_configMgr) {
        m_configMgr->stopWatching();
    }

    // 2. 终止目标进程
    if (m_processMgr) {
        m_processMgr->stop();
    }

    // 3. 释放单实例互斥体
    if (m_hMutex) {
        ReleaseMutex(m_hMutex);
        CloseHandle(m_hMutex);
        m_hMutex = nullptr;
    }

    // 4. 关闭日志系统（最后关闭）
    Logger::instance().shutdown();

    s_instance = nullptr;
}

// 查找配置文件路径
// 从 exe 所在目录向上逐级搜索 Config/config.json
std::wstring Application::findConfigPath() {
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    fs::path searchDir = fs::path(exePath).parent_path();

    for (int i = 0; i < 5; ++i) {
        fs::path candidate = searchDir / L"Config" / L"config.json";
        if (fs::exists(candidate)) {
            return candidate.wstring();
        }
        fs::path parent = searchDir.parent_path();
        if (parent == searchDir) break;
        searchDir = parent;
    }

    return L"";  // 未找到
}

// 解析目标程序路径
// 将配置中的相对路径转为绝对路径
// 从 exe 所在目录向上搜索目标文件，兼容不同深度的输出目录
std::wstring Application::resolveTargetPath(const std::string& relativePath) {
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    fs::path searchDir = fs::path(exePath).parent_path();
    std::wstring widePath(relativePath.begin(), relativePath.end());

    // 向上逐级搜索，直到找到目标文件或到达根目录
    for (int i = 0; i < 5; ++i) {
        fs::path candidate = searchDir / widePath;
        if (fs::exists(candidate)) {
            std::error_code ec;
            fs::path canonical = fs::canonical(candidate, ec);
            if (!ec) {
                return canonical.wstring();
            }
            return fs::absolute(candidate).wstring();
        }
        fs::path parent = searchDir.parent_path();
        if (parent == searchDir) break;
        searchDir = parent;
    }

    // 搜索失败，回退到基于 exe 目录的绝对路径
    fs::path exeDir = fs::path(exePath).parent_path();
    return fs::absolute(exeDir / widePath).wstring();
}

// 控制台事件处理器
// 处理系统关机、用户注销等事件
BOOL WINAPI Application::ctrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            LOG_INFO("收到退出信号（类型: %lu），正在退出...", ctrlType);
            if (s_instance) {
                s_instance->m_running = false;
            }
            return TRUE;
        default:
            return FALSE;
    }
}
