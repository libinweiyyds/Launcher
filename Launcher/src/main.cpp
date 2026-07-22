// Launcher 程序入口
// 启动 Application 生命周期管理器，进入主循环
// 编译为 Windows 子系统（无控制台窗口），使用 mainCRTStartup 入口点

#include "core/Application.h"

int main() {
    return Application::instance().run(GetModuleHandle(nullptr));
}
