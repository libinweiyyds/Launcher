#pragma once

#include <string>
#include <Windows.h>

// UTF-8 → 宽字符转换辅助函数
inline std::wstring utf8ToWide(const std::string& utf8) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wlen);
    return wide;
}

// 输出 UTF-8 调试信息到 VS 调试窗口（不乱码）
inline void wsDebug(const std::string& utf8) {
    OutputDebugStringW((L"[WS] " + utf8ToWide(utf8) + L"\n").c_str());
}
