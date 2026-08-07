#include "Application.h"
#include "../logger/Logger.h"
#include "../config/ConfigManager.h"
#include "../process/ProcessManager.h"
#include "../ipc/IpcBridge.h"
#include "../wsUtil/WebSocketClient.h"
#include "../update/VersionManager.h"
#include "../update/DownloadManager.h"
#include "../update/ZipManager.h"
#include "../update/UpdateUtils.h"
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

    // 5. 初始化更新模块（先于进程启动，因为要先检查更新）
    m_versionMgr = std::make_unique<VersionManager>();
    const VersionInfo& verInfo = m_configMgr->getVersion();
    m_versionMgr->setCurrent(verInfo.current);
    m_versionMgr->setHistory(verInfo.history);
    m_downloadMgr = std::make_unique<DownloadManager>();
    m_zipMgr = std::make_unique<ZipManager>();
    LOG_INFO("当前版本: %s", m_versionMgr->getCurrent().c_str());

    // 6. 获取服务端发布信息（只在 init 中查询一次，checkForUpdate 和下载复用）
    const auto& ws = m_configMgr->getWebsocket();
    const auto& sw = m_configMgr->getSoftware();
    auto pubInfo = UpdateUtils::fetchPublishInfo(ws.address, ws.port, sw.name);

    // 7. 检查更新
    checkForUpdate(pubInfo);

    // 8. 动态解析目标路径: versions/{currentVersion}/{exeName}
    std::string targetPath = "versions/" + m_versionMgr->getCurrent() + "/" + sw.exeName;
    m_currentTargetPath = resolveTargetPath(targetPath);

    if (!fs::exists(m_currentTargetPath)) {
        // 目标不存在 → 使用服务端推荐版本下载，而非本地配置版本
        std::string dlVersion = pubInfo.valid ? pubInfo.recommendVersion : m_versionMgr->getCurrent();
        int sid = pubInfo.valid ? pubInfo.softwareId : 0;
        int reserveNum = pubInfo.valid ? pubInfo.clientReserveNum : 2;
        LOG_WARN("目标程序不存在: %ls，自动下载版本 %s",
            m_currentTargetPath.c_str(), dlVersion.c_str());
        if (sid == 0) { LOG_ERROR("无法获取 softwareId，放弃下载"); return false; }
        doUpdate(dlVersion, sid, ws.address, ws.port, reserveNum);
        // 版本可能已变更，重新解析路径
        targetPath = "versions/" + m_versionMgr->getCurrent() + "/" + sw.exeName;
        m_currentTargetPath = resolveTargetPath(targetPath);
        if (!fs::exists(m_currentTargetPath)) {
            LOG_ERROR("下载后目标仍不存在: %ls", m_currentTargetPath.c_str());
            return false;
        }
    }

    // 9. 启动子进程（如果 checkForUpdate/doUpdate 已经启动了则跳过）
    if (!m_processMgr->isRunning()) {
        fs::path exeDir = fs::path(m_currentTargetPath).parent_path();
        std::wstring workingDir = exeDir.wstring();
        if (!m_processMgr->start(m_currentTargetPath, workingDir)) {
            LOG_ERROR("目标程序启动失败");
            return false;
        }
    } else {
        LOG_INFO("子进程已在运行，跳过启动");
    }

    // 10. 启动 IPC 服务端
    /*m_ipcBridge = std::make_unique<IpcBridge>();
    m_ipcBridge->setMessageCallback(
        [this](const std::string& msg) { onIpcMessage(msg); }
    );

    if (!m_ipcBridge->start("MyService")) {
        LOG_ERROR("IPC 服务启动失败");
        return false;
    }*/

    // 8. 连接 WebSocket
    /*m_wsClient = std::make_unique<WebSocketClient>();
    m_wsClient->onMessage(
        [this](int code, const std::string& type, const std::string& desc, const std::string& data) {
            onWsMessage(code, type, desc, data);
        }
    );

    const WebsocketConfig& wsCfg = m_configMgr->getWebsocket();
    LOG_INFO("正在连接 WebSocket: %s:%d/ws/%s/%d",
        wsCfg.address.c_str(), wsCfg.port, wsCfg.group.c_str(), wsCfg.id);
    m_wsClient->connect(wsCfg.group, wsCfg.id);*/


    m_running = true;
    return true;
}

// 主循环：同时等待子进程退出、配置文件变更、IPC 停止事件
void Application::mainLoop() {
    LOG_INFO("进入主循环");

    HANDLE hProcess = m_processMgr->getProcessHandle();
    HANDLE hConfig = m_configMgr->startWatching();
    HANDLE hIpcStop = m_ipcBridge ? m_ipcBridge->getStopEvent() : nullptr;

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
                const auto& sw = m_configMgr->getSoftware();
                std::string newTargetPath = "versions/" + m_configMgr->getVersion().current + "/" + sw.exeName;
                std::wstring newPath = resolveTargetPath(newTargetPath);

                if (newPath != m_currentTargetPath) {
                    if (!fs::exists(newPath)) { LOG_ERROR("新目标不存在: %ls", newPath.c_str()); continue; }
                    m_processMgr->stop();
                    fs::path exeDir = fs::path(newPath).parent_path();
                    if (m_processMgr->start(newPath, exeDir.wstring())) {
                        m_currentTargetPath = newPath;
                        hProcess = m_processMgr->getProcessHandle();
                        LOG_INFO("热加载完成");
                    } else {
                        m_running = false;
                    }
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

// 解析目标程序路径（相对于工作目录）
std::wstring Application::resolveTargetPath(const std::string& relativePath) {
    std::wstring widePath(relativePath.begin(), relativePath.end());
    std::error_code ec;
    fs::path absolute = fs::absolute(fs::path(widePath), ec);
    if (!ec) return absolute.wstring();
    return fs::path(widePath).wstring();
}

// IPC 消息回调
void Application::onIpcMessage(const std::string& message) {
    LOG_INFO("收到 IPC 消息: %s", message.c_str());
}

// WebSocket 消息回调
void Application::onWsMessage(int code, const std::string& type,
                               const std::string& desc, const std::string& data) {
    // 非 200 状态码忽略
   /* if (code != 200) {
		LOG_WARN("WS 收到非 200 消息（code=%d, type=%s），已忽略", code, type.c_str());
		return;
	}*/

    LOG_INFO("WS 收到消息: type=%s, desc=%s, data=%s",
        type.c_str(), desc.c_str(), data.c_str());

}

// 检查更新
// pubInfo 由调用方传入（init 中已查询），避免重复请求
void Application::checkForUpdate(const PublishInfo& info) {
    if (!info.valid) return;

    const auto& ws = m_configMgr->getWebsocket();
    const auto& sw = m_configMgr->getSoftware();

    // 校验本地版本号格式（按 "." 分割后至少 3 段），无效则用服务端版本覆盖
    auto validFormat = [](const std::string& v) {
        auto trimV = [](const std::string& s) {
            if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) return s.substr(1);
            return s;
        };
        std::string s = trimV(v);
        int dots = 0;
        for (char c : s) if (c == '.') ++dots;
        return dots >= 2;  // 至少 "1.0.0" 三段
    };

    bool localOk = validFormat(m_versionMgr->getCurrent());
    bool remoteOk = validFormat(info.recommendVersion);
    if (!localOk || !remoteOk) {
        LOG_WARN("版本号格式无效（local=%s, remote=%s），以服务端为准",
            m_versionMgr->getCurrent().c_str(), info.recommendVersion.c_str());
        if (remoteOk) {
            m_versionMgr->setCurrent(info.recommendVersion);
            m_versionMgr->setHistory({info.recommendVersion});
            if (!m_configMgr->getFilePath().empty()) {
                UpdateUtils::updateConfigVersion(
                    std::string(m_configMgr->getFilePath()),
                    info.recommendVersion,
                    {info.recommendVersion}
                );
            }
        }
        return;
    }

    int cmpMin = UpdateUtils::compareVersion(m_versionMgr->getCurrent(), info.minVersion);
    int cmpRec = UpdateUtils::compareVersion(m_versionMgr->getCurrent(), info.recommendVersion);

    // 检查目标 exe 是否实际存在（用于修复版本目录被误删的场景）
    std::string targetPath = "versions/" + m_versionMgr->getCurrent() + "/" + sw.exeName;
    bool targetExists = fs::exists(resolveTargetPath(targetPath));

    if (cmpMin < 0) {
        // 低于最低版本 → 强制升级
        if (showUpdateDialog(info, true)) {
            doUpdate(info.recommendVersion, info.softwareId,
                     ws.address, ws.port, info.clientReserveNum);
        }
    } else if (cmpRec < 0) {
        // 低于推荐版本 → 可选升级
        if (showUpdateDialog(info, false)) {
            doUpdate(info.recommendVersion, info.softwareId,
                     ws.address, ws.port, info.clientReserveNum);
        }
    } else if (cmpRec > 0) {
        // 本地版本高于推荐版本 → 强制回滚
        LOG_WARN("本地版本 %s 高于推荐版本 %s，执行回滚",
            m_versionMgr->getCurrent().c_str(), info.recommendVersion.c_str());
        doUpdate(info.recommendVersion, info.softwareId,
                 ws.address, ws.port, info.clientReserveNum);
    } else if (!targetExists) {
        // 版本号匹配但 exe 文件缺失（如被误删）→ 使用推荐版本修复
        LOG_WARN("版本号 %s 匹配但目标文件缺失，执行修复下载", info.recommendVersion.c_str());
        doUpdate(info.recommendVersion, info.softwareId,
                 ws.address, ws.port, info.clientReserveNum);
    }
}

// 显示更新弹窗
bool Application::showUpdateDialog(const PublishInfo& info, bool forced) {
    auto toWide = [](const std::string& s) -> std::wstring {
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring ws(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
        return ws;
    };

    std::wstring msg;
    if (forced) {
        msg = L"软件需要更新到最新版本才能继续使用。\n\n版本: " + toWide(info.recommendVersion);
    } else {
        std::wstring remark = toWide(info.remark);
        if (remark.empty()) remark = L"发现新版本，是否更新？";
        msg = remark + L"\n\n版本: " + toWide(info.recommendVersion);
    }

    std::wstring title = forced ? L"强制更新" : L"软件更新";
    UINT flags = MB_ICONINFORMATION | (forced ? MB_OK : MB_OKCANCEL);
    int result = MessageBoxW(nullptr, msg.c_str(), title.c_str(), flags);
    return (result == IDOK);
}

// 执行下载+解压+切换
void Application::doUpdate(const std::string& version, int softwareId,
                            const std::string& host, int port, int reserveNum) {
    LOG_INFO("开始更新: %s -> %s", m_versionMgr->getCurrent().c_str(), version.c_str());

    if (softwareId == 0) { LOG_ERROR("softwareId 为空"); return; }

    std::string expectedHash = UpdateUtils::fetchFileHash(host, port, softwareId, version);
    if (expectedHash.empty()) { LOG_ERROR("获取 Hash 失败"); return; }

    std::string url = UpdateUtils::buildDownloadUrl(host, port, softwareId, version);
    std::string zipTmp = "download/" + version + ".zip.tmp";
    std::string zipFinal = "download/" + version + ".zip";

    // 清理残留旧文件
    std::error_code ec;
    std::filesystem::remove(zipTmp, ec);
    std::filesystem::remove(zipFinal, ec);

    if (!m_downloadMgr->download(url, zipTmp)) { LOG_ERROR("下载失败"); return; }

    // 空文件检测
    auto fileSize = std::filesystem::file_size(zipTmp);
    if (fileSize == 0) { LOG_ERROR("下载文件为空"); std::filesystem::remove(zipTmp, ec); return; }
    LOG_INFO("下载完成: %llu bytes", fileSize);

    std::string actualHash = DownloadManager::sha256(zipTmp);
    if (actualHash != expectedHash) {
        LOG_ERROR("Hash mismatch: exp=%s, act=%s", expectedHash.c_str(), actualHash.c_str());
        std::filesystem::remove(zipTmp, ec); return;
    }
    LOG_INFO("SHA256 OK");

    std::filesystem::rename(zipTmp, zipFinal, ec);
    if (ec) { LOG_ERROR("rename failed"); return; }

    std::string tempDir = "temp/" + version;
    std::string versionsDir = "versions/" + version;
    // 清理残留
    if (std::filesystem::exists(tempDir)) std::filesystem::remove_all(tempDir, ec);
    if (!m_zipMgr->extract(zipFinal, tempDir)) {
        LOG_ERROR("extract failed");
        std::filesystem::remove_all(tempDir, ec); std::filesystem::remove(zipFinal, ec); return;
    }

    // 自动展开：如果解压后只有一个子文件夹，进入其内部
    std::string sourceDir = tempDir;
    int dirCount = 0, fileCount = 0;
    fs::path singleSubDir;
    for (const auto& entry : fs::directory_iterator(tempDir)) {
        if (entry.is_directory()) { ++dirCount; singleSubDir = entry.path(); }
        else ++fileCount;
    }
    if (dirCount == 1 && fileCount == 0) {
        sourceDir = singleSubDir.string();
        LOG_INFO("解压后自动展开: %s → %s", tempDir.c_str(), sourceDir.c_str());
    }

    std::filesystem::create_directories("versions", ec);
    if (std::filesystem::exists(versionsDir)) std::filesystem::remove_all(versionsDir, ec);

    std::filesystem::rename(sourceDir, versionsDir, ec);
    if (ec) {
        LOG_ERROR("rename failed: %s", ec.message().c_str());
        std::filesystem::remove_all(tempDir, ec); std::filesystem::remove(zipFinal, ec); return;
    }

    std::string newExe = VersionManager::findExeInDir(versionsDir);
    if (newExe.empty()) {
        LOG_ERROR("no exe found in update");
        std::filesystem::remove_all(versionsDir, ec); std::filesystem::remove(zipFinal, ec); return;
    }

    m_versionMgr->recordVersion(version);
    UpdateUtils::updateConfigVersion(
        std::string(m_configMgr->getFilePath()),
        version,
        m_versionMgr->getHistory()
    );

    m_processMgr->stop();
    std::wstring widePath(newExe.begin(), newExe.end());
    if (!m_processMgr->start(widePath, L"")) {
        LOG_ERROR("start new version failed, rolling back");
        const auto& hist = m_versionMgr->getHistory();
        if (hist.size() > 1 && !m_configMgr->getFilePath().empty()) {
            UpdateUtils::updateConfigVersion(
                std::string(m_configMgr->getFilePath()),
                hist[1],
                {hist.begin() + 1, hist.end()}
            );
        }
        return;
    }

    m_versionMgr->cleanup("versions", reserveNum);
    std::filesystem::remove(zipFinal, ec);
    LOG_INFO("update success: %s", version.c_str());
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
