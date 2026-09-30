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
#include "../update/ConfigSync.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <shellapi.h>
#include <algorithm>

namespace fs = std::filesystem;

// 静态成员初始化
Application* Application::s_instance = nullptr;

// 获取全局唯一实例
Application& Application::instance() {
    static Application inst;
    return inst;
}


Application::~Application() {
    shutdown();
}

// 程序入口
int Application::run(HINSTANCE hInstance) {
    s_instance = this;

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

// 初始化
// Logger → 单实例检查 → 加载配置 → 解析目标路径 → 启动子进程 → 启动 IPC
bool Application::init() {
    // 0. 主动设置工作目录为 exe 所在目录，避免快捷方式起始位置影响相对路径解析
    {
        wchar_t exePath[MAX_PATH] = { 0 };
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        fs::path exeDir = fs::path(exePath).parent_path();
        SetCurrentDirectoryW(exeDir.wstring().c_str());
    }

    // 1. 初始化日志系统
    if (!Logger::instance().init("./logs")) {
        return false;
    }

    LOG_INFO("日志系统初始化完成");

    // 2. 创建 ConfigManager
    m_configMgr = std::make_unique<ConfigManager>();

    // 3. 加载配置文件（必须先加载，否则 softwareName 是默认值）
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

    // 4. 单实例检查（Local\ + softwareName：每用户独立，避免多用户登录冲突）
    // 注意：必须先加载 config.json，否则 softwareName 是默认空值，mutex 名就会是 "Local\\"
    {
        std::wstring mutexName = L"Local\\" + utf8ToWide(m_configMgr->getSoftware().softwareName);
        if (mutexName == L"Local\\") {
            // softwareName 加载失败时不允许启动（避免空 mutex 名冲突）
            LOG_ERROR("softwareName 为空，无法创建唯一 mutex，拒绝启动");
            return false;
        }
        m_hMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
        if (m_hMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
            LOG_ERROR("检测到已有 Launcher 实例在运行（mutex=%ls），本次启动取消", mutexName.c_str());
            if (m_hMutex) {
                CloseHandle(m_hMutex);
                m_hMutex = nullptr;
            }
            return false;
        }
    }

    LOG_INFO("单实例检查通过");

    // 5. 创建进程管理器
    m_processMgr = std::make_unique<ProcessManager>();

    // 6. 初始化更新模块    检查更新等操作
    m_versionMgr = std::make_unique<VersionManager>();
    const VersionInfo& verInfo = m_configMgr->getVersion();
    m_versionMgr->setCurrent(verInfo.current);
    m_versionMgr->setHistory(verInfo.history);
    m_downloadMgr = std::make_unique<DownloadManager>();
    m_zipMgr = std::make_unique<ZipManager>();
    LOG_INFO("当前版本: %s", m_versionMgr->getCurrent().c_str());

    // 7. 获取服务端发布信息
    const auto& ws = m_configMgr->getWebsocket();
    const auto& sw = m_configMgr->getSoftware();
    auto pubInfo = UpdateUtils::fetchPublishInfo(ws.address, ws.port, sw.softwareName);

    // 记录服务端推荐版本（用于回滚约束：不允许启动比推荐版还新的版本）
    if (pubInfo.valid) {
        m_lastRecommendVersion = pubInfo.recommendVersion;
        m_lastSoftwareId = pubInfo.softwareId;
        m_lastReserveNum = pubInfo.clientReserveNum;
    }

    // 8. 检查更新
    checkForUpdate(pubInfo);

    // 9. 动态解析目标路径: versions/{currentVersion}/{exeName}
    std::string targetPath = "versions/" + m_versionMgr->getCurrent() + "/" + sw.exeName;
    m_currentTargetPath = resolveTargetPath(targetPath);

    if (!fs::exists(m_currentTargetPath)) {
        // 目标不存在
        // 使用服务端推荐版本下载，而非本地配置版本
        std::string dlVersion = pubInfo.valid ? pubInfo.recommendVersion : m_versionMgr->getCurrent();
        int sid = pubInfo.valid ? pubInfo.softwareId : 0;
        int reserveNum = pubInfo.valid ? pubInfo.clientReserveNum : 2;
        LOG_WARN("目标程序不存在: %ls，自动下载版本 %s",
            m_currentTargetPath.c_str(), dlVersion.c_str());
        if (sid == 0) { LOG_ERROR("无法获取 softwareId，放弃下载"); return false; }
        bool updateOk = doUpdate(dlVersion, sid, ws.address, ws.port, reserveNum);
        // 重新解析路径（doUpdate 成功后 m_versionMgr->current 已经是新版本）
        targetPath = "versions/" + m_versionMgr->getCurrent() + "/" + sw.exeName;
        m_currentTargetPath = resolveTargetPath(targetPath);
        if (!updateOk || !fs::exists(m_currentTargetPath)) {
            // 下载失败 → 尝试用本地历史版本启动（不设上限，避免降级场景下无可用版本）
            if (tryRollback("")) {
                // 校验回滚后的版本不超过 recommendVersion
                if (!m_lastRecommendVersion.empty() &&
                    UpdateUtils::compareVersion(m_versionMgr->getCurrent(),
                                                m_lastRecommendVersion) > 0) {
                    LOG_ERROR("回滚后版本仍高于服务端上限 %s",
                        m_lastRecommendVersion.c_str());
                    showStartupFailureDialog(L"启动失败：回滚版本仍超出服务端限制");
                    return false;
                }
                LOG_WARN("下载失败，已自动回滚到历史版本");
                showStartupFailureDialog(L"下载新版本失败，已自动回滚到旧版本继续运行");
                // 继续运行，不退出
            } else {
                // 本地无任何可用历史版本 → 兜底再下载一次服务端推荐版本
                if (!m_lastRecommendVersion.empty() && m_lastSoftwareId != 0) {
                    LOG_WARN("本地无可用历史版本，兜底下载服务端推荐版本: %s",
                        m_lastRecommendVersion.c_str());
                    bool fallbackOk = doUpdate(m_lastRecommendVersion, m_lastSoftwareId,
                                               ws.address, ws.port, m_lastReserveNum);
                    if (fallbackOk) {
                        // 重新解析路径
                        targetPath = "versions/" + m_versionMgr->getCurrent() + "/" + sw.exeName;
                        m_currentTargetPath = resolveTargetPath(targetPath);
                    } else {
                        LOG_ERROR("兜底下载失败");
                        showStartupFailureDialog(L"启动失败：无法下载新版本且无可用历史版本");
                        return false;
                    }
                } else {
                    LOG_ERROR("下载失败，且无可用历史版本");
                    showStartupFailureDialog(L"启动失败：无法下载新版本且无可用历史版本");
                    return false;
                }
            }
        }
    }

    // 10. 同步远程配置（必须有配置才能启动，失败则退出）
    if (!syncManagedConfig()) {
        LOG_ERROR("同步配置失败，启动终止");
        return false;
    }

    // 11. 启动子进程（已启动则跳过避免重复启动）
    if (!m_processMgr->isRunning()) {
        // 配置缺失闸门：缺失时自动重试 fetchRemoteConfig（两步 fallback），
        // 两端点都失败才拒绝启动（防止数据丢失）
        if (!ensureManagedConfigReady(m_currentTargetPath)) {
            showStartupFailureDialog(L"远程配置拉取失败，无法启动被管理软件");
            return false;
        }
        fs::path exeDir = fs::path(m_currentTargetPath).parent_path();
        std::wstring workingDir = exeDir.wstring();
        if (!m_processMgr->start(m_currentTargetPath, workingDir)) {
            LOG_ERROR("目标程序启动失败");
            return false;
        }
    } else {
        LOG_INFO("子进程已在运行，跳过启动");
    }

    // 12. 启动 IPC 服务端
    /*m_ipcBridge = std::make_unique<IpcBridge>();
    m_ipcBridge->setMessageCallback(
        [this](const std::string& msg) { onIpcMessage(msg); }
    );

    if (!m_ipcBridge->start("MyService")) {
        LOG_ERROR("IPC 服务启动失败");
        return false;
    }*/

    // 13. 连接 WebSocket
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
                    // 启动管理软件前先同步远程配置（失败不阻塞）
                    syncManagedConfig();
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

    // ★ 2.7 在终止子进程前上传配置变更
    // 先确认一致性再退出：若当前 hash 与 baseline 不同，POST 上传；成功则更新 baseline
    if (!m_currentTargetPath.empty() && m_configMgr) {
        const auto& ws = m_configMgr->getWebsocket();
        const auto& sw = m_configMgr->getSoftware();
        fs::path exeDir = fs::path(m_currentTargetPath).parent_path();
        std::string baseDir = exeDir.string();
        std::string relPath = sw.localFilePath.empty() ? "config/config.json" : sw.localFilePath;
        std::string hostName = UpdateUtils::getCurrentUserName() + "_" + ws.id;
        std::string baseline = m_configMgr->getSoftwareSha256();
        ConfigSync::uploadConfigIfChanged(
            ws.address, ws.port,
            sw.configId,
            hostName,
            baseDir,
            relPath,
            baseline,
            [this](const std::string& newHash) {
                m_configMgr->setSoftwareSha256(newHash);
                m_configMgr->writeConfigToDiskPublic();
            });
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
        // 低于最低版本 → 必须升级
        // forceUpgrade=1 强制直接更新，不弹窗；否则弹窗确认（仅确认按钮）
        if (info.forceUpgrade == 1 || showUpdateDialog(info, true)) {
            if (!doUpdate(info.recommendVersion, info.softwareId,
                          ws.address, ws.port, info.clientReserveNum)) {
                handleUpdateFailure();
                return;
            }
        }
    } else if (cmpRec < 0) {
        // 低于推荐版本 → 升级
        // forceUpgrade=1 强制直接更新，不弹窗；否则弹窗确认（可取消）
        if (info.forceUpgrade == 1 || showUpdateDialog(info, false)) {
            if (!doUpdate(info.recommendVersion, info.softwareId,
                          ws.address, ws.port, info.clientReserveNum)) {
                handleUpdateFailure();
                return;
            }
        }
    } else if (cmpRec > 0) {
        // 本地版本高于推荐版本 → 强制回滚到推荐版本
        LOG_WARN("本地版本 %s 高于推荐版本 %s，执行降级",
            m_versionMgr->getCurrent().c_str(), info.recommendVersion.c_str());
        if (!doUpdate(info.recommendVersion, info.softwareId,
                      ws.address, ws.port, info.clientReserveNum)) {
            handleUpdateFailure();
            return;
        }
    } else if (!targetExists) {
        // 版本号匹配但 exe 文件缺失（如被误删）→ 使用推荐版本修复
        LOG_WARN("版本号 %s 匹配但目标文件缺失，执行修复下载", info.recommendVersion.c_str());
        if (!doUpdate(info.recommendVersion, info.softwareId,
                      ws.address, ws.port, info.clientReserveNum)) {
            handleUpdateFailure();
            return;
        }
    }
}

// 更新失败后的统一处理：
// 1) 本地有可启动的历史版本 → 直接用 + 拉远程配置 + 启动（不重复下载）
// 2) 本地没有可启动版本 → 兜底再下载一次服务端 recommendVersion（启动管理软件前再次拉配置）
// 3) 兜底下载也失败 → 才弹窗 + 让主循环退出
// 注意：此处不再用 m_lastRecommendVersion 作为上限，
// 因为降级场景（远端 < 本地）下历史版本可能本来就 > 推荐版，必须允许回滚到本地可用版本。
void Application::handleUpdateFailure() {
    const std::string failedVer = m_versionMgr->getCurrent();
    const auto& ws = m_configMgr->getWebsocket();

    // 1) 本地有可启动历史版本 → tryRollback 内部已同步配置 + 启动
    if (tryRollback("")) {
        const std::string& newVer = m_versionMgr->getCurrent();
        LOG_WARN("更新失败: %s 启动失败 → 已用本地历史版本 %s 启动",
            failedVer.c_str(), newVer.c_str());
        std::wstring msg = L"更新失败: " + utf8ToWide(failedVer) +
                           L"\n\n已使用本地历史版本: " + utf8ToWide(newVer) +
                           L"\n\n继续运行";
        showStartupFailureDialog(msg);
        return;
    }

    // 2) 本地无可用 → 兜底再下载一次服务端 recommendVersion
    if (!m_lastRecommendVersion.empty() && m_lastSoftwareId != 0) {
        LOG_WARN("本地无可用历史版本，兜底下载服务端推荐版本: %s",
            m_lastRecommendVersion.c_str());
        // 通过 doUpdate 走完整下载+解压+切换流程（内部会自动拉配置 + 启动）
        if (doUpdate(m_lastRecommendVersion, m_lastSoftwareId,
                     ws.address, ws.port, m_lastReserveNum)) {
            return;  // 兜底成功
        }
    }

    // 3) 兜底失败 → 真正无路可走
    LOG_ERROR("更新失败: %s 启动失败，且无可用历史版本与兜底下载", failedVer.c_str());
    showStartupFailureDialog(L"启动失败：无法启动新版本且无可用历史版本");
    m_running = false;  // 通知主循环退出
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

// 显示启动失败弹窗（仅确认按钮），通知用户当前问题
void Application::showStartupFailureDialog(const std::wstring& reason) {
    MessageBoxW(nullptr, reason.c_str(), L"启动失败", MB_ICONWARNING | MB_OK);
}

// 启动前校验被管理软件配置文件存在
// 流程：
//   1. 检查本地配置文件是否存在
//   2. 不存在 + allowRetry → 触发 fetchRemoteConfig（内部两步 fallback：
//        先 fetchClientSoftwareConfig → 失败 → fetchDefaultConfig）
//      两步都失败 → return false（弹"远程配置拉取失败，无法启动"）
//   3. 拉取成功 → 写入磁盘（inheritAndMergeConfig），二次校验
//   4. 不存在 + !allowRetry → 直接 return false
// localFilePath 为空时 fallback 到 "config/config.json"
bool Application::ensureManagedConfigReady(const std::wstring& targetPath, bool allowRetry) {
    if (targetPath.empty()) {
        LOG_ERROR("ensureManagedConfigReady: 目标路径为空");
        return false;
    }
    const auto& sw = m_configMgr->getSoftware();
    fs::path exeDir = fs::path(targetPath).parent_path();
    // 与 shutdown() 第 343 行保持一致的 fallback 约定
    std::string relPath = sw.localFilePath.empty() ? "config/config.json" : sw.localFilePath;
    fs::path localConfigFull = exeDir / relPath;

    // 1. 文件已存在 → 通过
    if (fs::exists(localConfigFull)) {
        return true;
    }

    // 2. 文件缺失
    LOG_WARN("被管理软件配置文件不存在: %ls", localConfigFull.wstring().c_str());

    // 2a. 不允许重试 → 直接拒绝
    if (!allowRetry) {
        return false;
    }

    // 2b. 允许重试 → 拉取远程配置（fetchRemoteConfig 内部两步 fallback）
    const auto& websock = m_configMgr->getWebsocket();
    const std::string& ver = m_versionMgr->getCurrent();
    std::string hostName = UpdateUtils::getCurrentUserName() + "_" + websock.id;
    RemoteConfig remote = UpdateUtils::fetchRemoteConfig(
            websock.address, websock.port,
            hostName, sw.softwareName, ver);
    if (!remote.valid) {
        // fetchRemoteConfig 已经两步 fallback，这里说明 ClientSoftwareConfig + DefaultConfig 都失败
        LOG_ERROR("远程配置拉取失败（ClientSoftwareConfig + DefaultConfig 均失败）");
        return false;
    }

    // 3. 写入磁盘（inheritAndMergeConfig 内部处理首次安装 / 已有 config 目录）
    if (!ConfigSync::inheritAndMergeConfig(
            remote, exeDir.wstring(),
            m_versionMgr->getHistory(),
            L"versions",
            ver,
            nullptr)) {
        LOG_ERROR("配置写入失败（inheritAndMergeConfig 返回 false）");
        return false;
    }

    // 4. 二次校验
    if (!fs::exists(localConfigFull)) {
        LOG_ERROR("配置写入后文件仍不存在: %ls", localConfigFull.wstring().c_str());
        return false;
    }

    LOG_INFO("被管理软件配置文件已重新拉取: %ls", localConfigFull.wstring().c_str());
    return true;
}

// 拉取远程配置并写入管理软件 exe 的相对路径
// 返回 false 表示失败（需要弹窗并终止启动流程）
bool Application::syncManagedConfig() {
    if (m_currentTargetPath.empty()) {
        LOG_WARN("syncManagedConfig: 当前目标路径为空，跳过");
        return false;
    }

    const auto& ws = m_configMgr->getWebsocket();
    const auto& sw = m_configMgr->getSoftware();
    const std::string& ver = m_versionMgr->getCurrent();

    // 先查询远程配置（fetchRemoteConfig 两步 fallback）
    std::string hostName = UpdateUtils::getCurrentUserName() + "_" + ws.id;
    RemoteConfig remote = UpdateUtils::fetchRemoteConfig(ws.address, ws.port,
                                           hostName, sw.softwareName, ver);
    if (!remote.valid) {
        LOG_WARN("syncManagedConfig: 获取远程配置失败");
        showStartupFailureDialog(L"获取最新配置文件失败，以旧配置启动");
        // 不阻塞启动：以旧配置继续
        return true;
    }

    // 计算目标路径（基于 remote.localFilePath）
    fs::path exeDir = fs::path(m_currentTargetPath).parent_path();
    std::string cleanPath = remote.localFilePath;
    while (cleanPath.size() >= 3 && cleanPath.substr(0, 3) == "../") cleanPath = cleanPath.substr(3);
    if (cleanPath.size() >= 2 && cleanPath.substr(0, 2) == "./") cleanPath = cleanPath.substr(2);
    fs::path localFileFull = exeDir / cleanPath;

    // 写入 configId 和 localFilePath（始终写入，用于 uploadConfigIfChanged 拼接路径）
    m_configMgr->setSoftwareConfigId(remote.configId);
    m_configMgr->setSoftwareLocalFilePath(cleanPath);

    std::wstring baseDir = exeDir.wstring();

    // 始终调用 inheritAndMergeConfig：内部判断 config 目录是否存在
    // - 不存在：遍历 history 找旧版本复制，再合并 remote.content
    // - 已存在：跳过复制，直接合并
    // 注意：首次安装回调不再写 sha256，保持为空，让退出时天然上传一次
    bool mergeOk = ConfigSync::inheritAndMergeConfig(
            remote, baseDir,
            m_versionMgr->getHistory(),
            L"versions",
            ver,
            // 全新安装回调（空实现：保持 sha256 为空）
            nullptr);

    if (!mergeOk) {
        LOG_ERROR("syncManagedConfig: 继承合并失败");
        showStartupFailureDialog(L"同步最新配置文件失败，以旧配置启动");
        return true;
    }

    // 合并后：首次安装保持 sha256 为空，强制首次退出时上传一次
    // 非首次：与 sha256 比对决定是否需要重传
    const std::string& baseline = m_configMgr->getSoftwareSha256();
    if (fs::exists(localFileFull)) {
        std::string curHash = DownloadManager::sha256(localFileFull.string());
        if (baseline.empty()) {
            // 首次安装：保持 sha256 为空，不写盘
            // 退出时 uploadConfigIfChanged 会比对：空 != curHash，强制上传
            LOG_INFO("syncManagedConfig: 首次安装，sha256 保持为空，强制退出时上传（hash=%s）",
                curHash.substr(0, 8).c_str());
        } else if (curHash == baseline) {
            // sha256 匹配：已同步
            LOG_INFO("syncManagedConfig: sha256 匹配（%s），已同步", curHash.substr(0, 8).c_str());
        } else {
            // sha256 不匹配：文件被改动过，不改动 sha256，退出时天然上传
            LOG_INFO("syncManagedConfig: sha256 不匹配（文件=%s, 基准=%s），启动前不改",
                curHash.substr(0, 8).c_str(), baseline.substr(0, 8).c_str());
        }
    }
    LOG_INFO("syncManagedConfig: 远程配置已同步完成");
    return true;
}

// UTF-8 字符串 → 宽字符串（用于中文显示）
std::wstring Application::utf8ToWide(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

// 用 history 中的旧版本尝试启动
// 跳过当前失败版本；返回 true 表示启动成功（m_currentTargetPath/m_current 已更新）
// maxAllowedVersion 非空时，跳过所有版本号 > maxAllowedVersion 的历史版本
// （远程控制回滚场景：不允许启动比服务端推荐版本还新的版本）
bool Application::tryRollback(const std::string& maxAllowedVersion) {
    const auto& history = m_versionMgr->getHistory();
    const auto& sw = m_configMgr->getSoftware();
    const std::string failedVer = m_versionMgr->getCurrent();

    LOG_WARN("开始回滚，当前版本 %s 不可用，尝试启动历史版本%s",
        failedVer.c_str(),
        maxAllowedVersion.empty() ? "" : ("（限制 ≤ " + maxAllowedVersion + "）").c_str());

    for (const auto& ver : history) {
        // 跳过当前失败版本，避免重复尝试
        if (ver == failedVer) continue;

        // 跳过超出服务端允许上限的版本（远程控制回滚场景）
        if (!maxAllowedVersion.empty() &&
            UpdateUtils::compareVersion(ver, maxAllowedVersion) > 0) {
            LOG_WARN("历史版本 %s 高于服务端上限 %s，跳过",
                ver.c_str(), maxAllowedVersion.c_str());
            continue;
        }

        std::string targetRel = "versions/" + ver + "/" + sw.exeName;
        std::wstring targetAbs = resolveTargetPath(targetRel);

        if (!fs::exists(targetAbs)) {
            LOG_WARN("历史版本 %s 目录不存在，跳过", ver.c_str());
            continue;
        }

        std::wstring widePath(targetAbs.begin(), targetAbs.end());
        // 工作目录：版本目录（保证管理软件读 ./config/config.ini 正确）
        fs::path rollDir = fs::path(targetAbs).parent_path();
        // 配置缺失闸门：跳过此版本，试下一个，m_currentTargetPath 不变
        // allowRetry=false：m_versionMgr->getCurrent() 仍是失败版本，不能直接重试
        if (!ensureManagedConfigReady(widePath, /*allowRetry=*/false)) {
            LOG_WARN("历史版本 %s 配置不存在，跳过", ver.c_str());
            continue;
        }
        if (!m_processMgr->start(widePath, rollDir.wstring())) {
            LOG_WARN("历史版本 %s 启动失败，跳过", ver.c_str());
            continue;
        }

        // 启动成功 → 更新内部状态与 config
        m_currentTargetPath = targetAbs;
        m_versionMgr->setCurrent(ver);  // current 指向回滚的旧版本
        // 重新组织 history：把 ver 移到最前面（移除原位置）
        std::vector<std::string> newHist = history;
        newHist.erase(std::remove(newHist.begin(), newHist.end(), ver), newHist.end());
        newHist.insert(newHist.begin(), ver);
        m_versionMgr->setHistory(newHist);

        if (!m_configMgr->getFilePath().empty()) {
            UpdateUtils::updateConfigVersion(
                std::string(m_configMgr->getFilePath()),
                ver,
                newHist
            );
        }

        // 启动管理软件前先同步远程配置（失败不阻塞）
        syncManagedConfig();

        LOG_WARN("回滚成功: %s (失败) → %s (已启动)，history 已同步",
            failedVer.c_str(), ver.c_str());
        return true;
    }

    LOG_ERROR("回滚失败: %s 及所有历史版本均不可用%s",
        failedVer.c_str(),
        maxAllowedVersion.empty() ? "" : ("（限制 ≤ " + maxAllowedVersion + "）").c_str());
    return false;
}

// 执行下载+解压+切换
bool Application::doUpdate(const std::string& version, int softwareId,
                            const std::string& host, int port, int reserveNum) {
    LOG_INFO("开始更新: %s -> %s", m_versionMgr->getCurrent().c_str(), version.c_str());

    if (softwareId == 0) { LOG_ERROR("softwareId 为空"); return false; }

    std::string expectedHash = UpdateUtils::fetchFileHash(host, port, softwareId, version);
    if (expectedHash.empty()) { LOG_ERROR("获取 Hash 失败"); return false; }

    std::string url = UpdateUtils::buildDownloadUrl(host, port, softwareId, version);
    std::string zipTmp = "download/" + version + ".zip.tmp";
    std::string zipFinal = "download/" + version + ".zip";

    // 清理残留旧文件
    std::error_code ec;
    std::filesystem::remove(zipTmp, ec);
    std::filesystem::remove(zipFinal, ec);

    if (!m_downloadMgr->download(url, zipTmp)) { LOG_ERROR("下载失败"); return false; }

    // 空文件检测
    auto fileSize = std::filesystem::file_size(zipTmp);
    if (fileSize == 0) { LOG_ERROR("下载文件为空"); std::filesystem::remove(zipTmp, ec); return false; }
    LOG_INFO("下载完成: %llu bytes", fileSize);

    std::string actualHash = DownloadManager::sha256(zipTmp);
    if (actualHash != expectedHash) {
        LOG_ERROR("Hash mismatch: exp=%s, act=%s", expectedHash.c_str(), actualHash.c_str());
        std::filesystem::remove(zipTmp, ec); return false;
    }
    LOG_INFO("SHA256 OK");

    std::filesystem::rename(zipTmp, zipFinal, ec);
    if (ec) { LOG_ERROR("rename failed"); std::filesystem::remove(zipFinal, ec); return false; }

    std::string tempDir = "temp/" + version;
    std::string versionsDir = "versions/" + version;
    // 清理残留
    if (std::filesystem::exists(tempDir)) std::filesystem::remove_all(tempDir, ec);
    if (!m_zipMgr->extract(zipFinal, tempDir)) {
        LOG_ERROR("extract failed");
        std::filesystem::remove_all(tempDir, ec); std::filesystem::remove(zipFinal, ec); return false;
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
        std::filesystem::remove_all(tempDir, ec); std::filesystem::remove(zipFinal, ec); return false;
    }

    std::string newExe = VersionManager::findExeInDir(versionsDir);
    if (newExe.empty()) {
        LOG_ERROR("no exe found in update");
        std::filesystem::remove_all(versionsDir, ec); std::filesystem::remove(zipFinal, ec); return false;
    }

    m_versionMgr->recordVersion(version);

    m_processMgr->stop();
    std::wstring widePath(newExe.begin(), newExe.end());
    // 更新 m_currentTargetPath 用于 syncManagedConfig 解析父目录
    m_currentTargetPath = widePath;
    // 工作目录：版本目录（保证管理软件读 ./config/config.ini 正确）
    fs::path newExeDir = fs::path(widePath).parent_path();
    // 启动管理软件前先同步远程配置
    syncManagedConfig();
    // 配置缺失闸门：失败则返回 false，触发 handleUpdateFailure → tryRollback
    // allowRetry=false：syncManagedConfig 刚刚失败过，重试同一端点无意义
    if (!ensureManagedConfigReady(widePath, /*allowRetry=*/false)) {
        return false;
    }
    if (!m_processMgr->start(widePath, newExeDir.wstring())) {
        LOG_ERROR("start new version failed, rolling back");
        m_processMgr->stop();
        // 启动失败时，回滚到 history[1] 仍由调用方 tryRollback 处理
        return false;
    }

    // cleanup 后再写 config，保证 history 与磁盘目录一致
    m_versionMgr->cleanup("versions", reserveNum);
    UpdateUtils::updateConfigVersion(
        std::string(m_configMgr->getFilePath()),
        version,
        m_versionMgr->getHistory()
    );

    std::filesystem::remove(zipFinal, ec);
    LOG_INFO("update success: %s", version.c_str());
    return true;
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
