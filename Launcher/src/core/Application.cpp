#include "Application.h"
#include "../logger/Logger.h"
#include "../config/ConfigManager.h"
#include "../process/ProcessManager.h"
#include "../ipc/IpcBridge.h"
#include "../wsUtil/WebSocketClient.h"
#include "../update/VersionManager.h"
#include "../update/DownloadManager.h"
#include "../update/ZipManager.h"
#include <nlohmann/json.hpp>
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
int Application::run(HINSTANCE hInstance) {
    s_instance = this;

    // Debug 模式分配控制台，便于查看日志输出
#ifdef _DEBUG
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
#endif

    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    if (!init()) {
        LOG_ERROR("初始化失败，程序即将退出");
        shutdown();
        return 1;
    }

    LOG_INFO("Launcher 启动完成（PID: %lu）", GetCurrentProcessId());

    mainLoop();

    shutdown();
    return 0;
}

// 初始化阶段
// Logger → 单实例检查 → 加载配置 → 解析目标路径 → 启动子进程 → 启动 IPC
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
        int len = WideCharToMultiByte(CP_UTF8, 0, configPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string configPathStr(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, configPath.c_str(), -1, &configPathStr[0], len, nullptr, nullptr);
        m_configMgr->load(configPathStr);
    } else {
        LOG_WARN("未找到 Config/config.json，使用默认配置");
        m_configMgr->load("");
    }

    // 5. 解析目标程序路径
    const TargetConfig& target = m_configMgr->getTarget();
    m_currentTargetPath = resolveTargetPath(target.path);

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

    // 7. 启动 IPC 服务端
    m_ipcBridge = std::make_unique<IpcBridge>();
    m_ipcBridge->setMessageCallback(
        [this](const std::string& msg) { onIpcMessage(msg); }
    );

    if (!m_ipcBridge->start("MyService")) {
        LOG_ERROR("IPC 服务启动失败");
        return false;
    }

    // 8. 连接 WebSocket
    m_wsClient = std::make_unique<WebSocketClient>();
    m_wsClient->onMessage(
        [this](int code, const std::string& type, const std::string& desc, const std::string& data) {
            onWsMessage(code, type, desc, data);
        }
    );

    const WebsocketConfig& wsCfg = m_configMgr->getWebsocket();
    LOG_INFO("正在连接 WebSocket: %s:%d/ws/%s/%d",
        wsCfg.address.c_str(), wsCfg.port, wsCfg.group.c_str(), wsCfg.id);
    m_wsClient->connect(wsCfg.group, wsCfg.id);

    // 9. 初始化更新模块
    m_versionMgr = std::make_unique<VersionManager>();
    const VersionInfo& verInfo = m_configMgr->getVersion();
    m_versionMgr->setCurrent(verInfo.current);
    m_versionMgr->setHistory(verInfo.history);
    m_downloadMgr = std::make_unique<DownloadManager>();
    m_zipMgr = std::make_unique<ZipManager>();

    LOG_INFO("当前版本: %s", m_versionMgr->getCurrent().c_str());

    m_running = true;
    return true;
}

// 主循环：同时等待子进程退出、配置文件变更、IPC 停止事件
void Application::mainLoop() {
    LOG_INFO("进入主循环");

    HANDLE hProcess = m_processMgr->getProcessHandle();
    HANDLE hConfig = m_configMgr->startWatching();
    HANDLE hIpcStop = m_ipcBridge->getStopEvent();

    while (m_running) {
        HANDLE handles[3];
        DWORD count = 0;

        handles[count++] = hProcess;    // 索引 0：子进程句柄

        if (hConfig != nullptr) {
            handles[count++] = hConfig; // 索引 1：配置文件变更通知
        }

        if (hIpcStop != nullptr) {
            handles[count++] = hIpcStop; // 索引 2：IPC 停止事件
        }

        // 用超时替代 INFINITE，确保周期性调用 WebSocket service()
        DWORD result = WaitForMultipleObjects(count, handles, FALSE, 200);

        // 无论哪个事件触发，都主动检查子进程是否还在运行
        // 避免因其他事件抢先导致进程退出事件被漏掉
        if (!m_processMgr->isRunning()) {
            LOG_INFO("目标程序已退出，Launcher 将退出");
            m_running = false;
            break;
        }

        if (result == WAIT_OBJECT_0 + 1) {
            // 配置文件变更 → 热加载
            LOG_INFO("检测到配置文件变更");
            if (m_configMgr->checkAndReload()) {
                const TargetConfig& target = m_configMgr->getTarget();
                std::wstring newPath = resolveTargetPath(target.path);

                if (newPath != m_currentTargetPath) {
                    LOG_INFO("目标程序路径已变更: %ls → %ls",
                        m_currentTargetPath.c_str(), newPath.c_str());

                    if (!fs::exists(newPath)) {
                        LOG_ERROR("新目标程序不存在: %ls，保持当前进程", newPath.c_str());
                        continue;
                    }

                    m_processMgr->stop();

                    std::wstring newWorkingDir;
                    if (!target.workingDir.empty()) {
                        newWorkingDir = std::wstring(target.workingDir.begin(),
                            target.workingDir.end());
                    }

                    if (m_processMgr->start(newPath, newWorkingDir)) {
                        m_currentTargetPath = newPath;
                        hProcess = m_processMgr->getProcessHandle();
                        LOG_INFO("热加载完成，新目标程序已启动");
                    } else {
                        LOG_ERROR("热加载失败：新目标程序启动失败，Launcher 将退出");
                        m_running = false;
                    }
                } else {
                    LOG_INFO("目标程序路径未变化，无需重启");
                }
            }
        } else if (result == WAIT_OBJECT_0 + 2) {
            // IPC 停止事件
            LOG_WARN("IPC 服务异常停止");
        }

        // 驱动 WebSocket 事件循环（非阻塞轮询）
        if (m_wsClient) {
            m_wsClient->service();
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

    // 2. 停止 IPC 服务
    if (m_ipcBridge) {
        m_ipcBridge->stop();
    }

    // 2.5 断开 WebSocket
    if (m_wsClient) {
        m_wsClient->disconnect();
    }

    // 3. 终止目标进程
    if (m_processMgr) {
        m_processMgr->stop();
    }

    // 4. 释放单实例互斥体
    if (m_hMutex) {
        ReleaseMutex(m_hMutex);
        CloseHandle(m_hMutex);
        m_hMutex = nullptr;
    }

    // 5. 关闭日志系统（最后关闭）
    Logger::instance().shutdown();

    s_instance = nullptr;
}

// 查找配置文件路径
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

    return L"";
}

// 解析目标程序路径
std::wstring Application::resolveTargetPath(const std::string& relativePath) {
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    fs::path searchDir = fs::path(exePath).parent_path();
    std::wstring widePath(relativePath.begin(), relativePath.end());

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

    fs::path exeDir = fs::path(exePath).parent_path();
    return fs::absolute(exeDir / widePath).wstring();
}

// IPC 消息回调
void Application::onIpcMessage(const std::string& message) {
    LOG_INFO("收到 IPC 消息: %s", message.c_str());
}

// WebSocket 消息回调
void Application::onWsMessage(int code, const std::string& type,
                               const std::string& desc, const std::string& data) {
    // 非 200 状态码忽略
    if (code != 200) {
        LOG_WARN("WS 收到非 200 消息（code=%d, type=%s），已忽略", code, type.c_str());
        return;
    }

    LOG_INFO("WS 收到消息: type=%s, desc=%s, data=%s",
        type.c_str(), desc.c_str(), data.c_str());

    // 处理更新指令
    if (type == "update") {
        try {
            auto j = nlohmann::json::parse(data);
            std::string version = j.value("version", "");
            std::string url = j.value("url", "");
            std::string hash = j.value("hash", "");
            handleWsUpdate(version, url, hash);
        } catch (...) {
            LOG_ERROR("update 指令 data 解析失败: %s", data.c_str());
        }
    }
}

// 处理更新指令
void Application::handleWsUpdate(const std::string& version,
                                  const std::string& url,
                                  const std::string& hash) {
    // 1. 检查是否需要更新
    if (!m_versionMgr->needUpdate(version)) {
        LOG_INFO("已是最新版本: %s", version.c_str());
        return;
    }

    LOG_INFO("开始更新: %s → %s", m_versionMgr->getCurrent().c_str(), version.c_str());

    // 2. 下载
    std::string zipTmp = "download/" + version + ".zip.tmp";
    std::string zipFinal = "download/" + version + ".zip";
    if (!m_downloadMgr->download(url, zipTmp)) {
        LOG_ERROR("下载失败");
        return;
    }

    // 3. 校验 Hash
    if (!hash.empty()) {
        std::string fileHash = DownloadManager::sha256(zipTmp);
        if (fileHash != hash) {
            LOG_ERROR("Hash 校验失败: 期望=%s, 实际=%s", hash.c_str(), fileHash.c_str());
            std::error_code ec;
            std::filesystem::remove(zipTmp, ec);
            return;
        }
        LOG_INFO("Hash 校验通过: %s", hash.c_str());
    }

    // 重命名 .tmp → .zip
    std::error_code ec;
    std::filesystem::rename(zipTmp, zipFinal, ec);
    if (ec) {
        LOG_ERROR("重命名失败: %s → %s", zipTmp.c_str(), zipFinal.c_str());
        return;
    }

    // 4. 解压到临时目录
    std::string tempDir = "temp/" + version;
    std::string versionsDir = "versions/" + version;
    if (!m_zipMgr->extract(zipFinal, tempDir)) {
        LOG_ERROR("解压失败，回滚");
        std::filesystem::remove_all(tempDir, ec);
        std::filesystem::remove(zipFinal, ec);
        return;
    }

    // 5. 原子移动 temp → versions
    std::filesystem::create_directories("versions", ec);
    std::filesystem::rename(tempDir, versionsDir, ec);
    if (ec) {
        LOG_ERROR("原子移动失败，清理: %s → %s", tempDir.c_str(), versionsDir.c_str());
        std::filesystem::remove_all(tempDir, ec);
        std::filesystem::remove(zipFinal, ec);
        return;
    }

    // 6. 查找 exe
    std::string newExePath = VersionManager::findExeInDir(versionsDir);
    if (newExePath.empty()) {
        LOG_ERROR("更新包中未找到可执行文件，回滚");
        std::filesystem::remove_all(versionsDir, ec);
        std::filesystem::remove(zipFinal, ec);
        return;
    }

    // 7. 记录版本 + 切换
    m_versionMgr->recordVersion(version);

    // 停止当前进程
    m_processMgr->stop();

    // 启动新版本
    std::wstring widePath(newExePath.begin(), newExePath.end());
    if (!m_processMgr->start(widePath, L"")) {
        LOG_ERROR("新版本启动失败，回滚至旧版本");
        // 切回上一个版本
        const auto& history = m_versionMgr->getHistory();
        if (history.size() > 1) {
            std::string rollbackDir = "versions/" + history[1];
            std::string rollbackExe = VersionManager::findExeInDir(rollbackDir);
            if (!rollbackExe.empty()) {
                std::wstring wideRollback(rollbackExe.begin(), rollbackExe.end());
                m_processMgr->start(wideRollback, L"");
            }
        }
        return;
    }

    // 8. 清理旧版本
    m_versionMgr->cleanup("versions", 2);
    std::filesystem::remove(zipFinal, ec);

    LOG_INFO("更新成功: %s", version.c_str());
}

// 控制台事件处理器
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
