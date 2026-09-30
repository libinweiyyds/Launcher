#pragma once

#include <Windows.h>
#include <string>
#include <memory>

class ProcessManager;
class ConfigManager;
class IpcBridge;
class WebSocketClient;
class VersionManager;
class DownloadManager;
class ZipManager;
struct PublishInfo;

// 应用程序生命周期管理器（单例）
// 负责：初始化日志 → 单实例检查 → 加载配置 → 启动子进程 → IPC服务 → 主循环 → 优雅退出
// 主循环同时监听子进程退出、配置文件变更、IPC 停止事件
class Application {
public:
    // 获取全局唯一实例
    static Application& instance();

    // 启动应用程序，进入主循环
    // 返回值：进程退出码（0 正常，非 0 异常）
    int run(HINSTANCE hInstance);

private:
    Application() = default;
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 初始化阶段：日志 → 单实例 → 配置 → 启动子进程 → IPC
    bool init();

    // 主循环：WaitForMultipleObjects 等待子进程退出 + 配置变更 + IPC 停止
    void mainLoop();

    // 清理阶段：终止子进程 → 释放资源 → 关闭日志
    void shutdown();

    // 查找配置文件路径（从 exe 目录向上搜索 Config/config.json）
    std::wstring findConfigPath();

    // 解析目标程序路径（相对→绝对）
    std::wstring resolveTargetPath(const std::string& relativePath);

    // IPC 消息回调
    void onIpcMessage(const std::string& message);

    // WebSocket 消息回调
    void onWsMessage(int code, const std::string& type,
                     const std::string& desc, const std::string& data);

    // 检查更新（启动时调用，pubInfo 由 init 中提前查询传入）
    void checkForUpdate(const PublishInfo& info);

    // 显示更新弹窗，forced=true 只有确认按钮
    bool showUpdateDialog(const PublishInfo& info, bool forced);

    // 执行下载+解压+切换流程
    // 返回 true 表示新版本启动成功；false 表示启动失败（调用方应尝试回滚或弹窗）
    bool doUpdate(const std::string& version, int softwareId,
                  const std::string& host, int port, int reserveNum);

    // 用 history 中的旧版本尝试启动（跳过当前失败版本）
    // maxAllowedVersion 非空时跳过所有版本号 > maxAllowedVersion 的历史版本
    // 返回 true 表示启动成功；false 表示无可用历史版本
    bool tryRollback(const std::string& maxAllowedVersion = "");

    // 当前流程检查到的服务端推荐版本（用于回滚时约束"不许启动比推荐版还新的版本"）
    std::string m_lastRecommendVersion;

    // 当前流程检查到的服务端 softwareId（兜底下载用，handleUpdateFailure 不需要重新请求发布信息）
    int m_lastSoftwareId = 0;

    // 当前流程检查到的服务端 clientReserveNum（兜底下载的清理保留数）
    int m_lastReserveNum = 2;

    // 显示启动失败弹窗（仅确认按钮）
    void showStartupFailureDialog(const std::wstring& reason);

    // 启动前校验被管理软件配置文件存在
    // targetPath: 待启动的目标 exe 路径（不同调用方路径来源不同：init 用 m_currentTargetPath，
    //             mainLoop 热加载用 newPath，tryRollback 用 targetAbs，doUpdate 用 m_currentTargetPath）
    // allowRetry: true = 缺失时尝试 fetchRemoteConfig 重新拉取；false = 仅检查
    // 返回 false 表示配置缺失或拉取失败（防止 syncManagedConfig 静默失败后无配置启动）
    bool ensureManagedConfigReady(const std::wstring& targetPath, bool allowRetry = true);

    // 拉取并写入远程配置到管理软件 exe 的相对路径下
    // 返回 true 表示成功，false 表示失败（弹窗后终止启动流程）
    bool syncManagedConfig();

    // 更新失败后的统一处理：先回滚，回滚失败再弹窗 + 让主循环退出
    void handleUpdateFailure();

    // UTF-8 → Wide 转换（中文不乱码）
    static std::wstring utf8ToWide(const std::string& s);

    // 控制台事件回调（处理系统关机、用户注销等）
    static BOOL WINAPI ctrlHandler(DWORD ctrlType);

    // 静态指针，用于在控制台回调中访问实例
    static Application* s_instance;

    bool m_running = false;
    HANDLE m_hMutex = nullptr;                                  // 单实例互斥体句柄
    std::unique_ptr<ConfigManager> m_configMgr;                 // 配置管理器
    std::unique_ptr<ProcessManager> m_processMgr;               // 进程管理器
    std::unique_ptr<IpcBridge> m_ipcBridge;                     // IPC 通信桥接
    std::unique_ptr<WebSocketClient> m_wsClient;                // WebSocket 客户端
    std::unique_ptr<VersionManager> m_versionMgr;               // 版本管理器
    std::unique_ptr<DownloadManager> m_downloadMgr;             // 下载管理器
    std::unique_ptr<ZipManager> m_zipMgr;                      // 解压管理器
    std::wstring m_currentTargetPath;                           // 当前生效的目标程序路径
};
