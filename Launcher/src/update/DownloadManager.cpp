#include "DownloadManager.h"
#include <Windows.h>
#include <winhttp.h>
#include "../logger/Logger.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <wincrypt.h>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

static bool parseUrl(const std::string& url, std::wstring& host, std::wstring& path, int& port) {
    std::string urlCopy = url;
    bool https = false;
    if (urlCopy.find("https://") == 0) { https = true; urlCopy = urlCopy.substr(8); port = 443; }
    else if (urlCopy.find("http://") == 0) { urlCopy = urlCopy.substr(7); port = 80; }
    else return false;

    size_t slashPos = urlCopy.find('/');
    std::string hostStr;
    if (slashPos != std::string::npos) { hostStr = urlCopy.substr(0, slashPos); path.assign(urlCopy.begin() + slashPos, urlCopy.end()); }
    else { hostStr = urlCopy; path = L"/"; }

    size_t colonPos = hostStr.find(':');
    if (colonPos != std::string::npos) { port = std::stoi(hostStr.substr(colonPos + 1)); hostStr = hostStr.substr(0, colonPos); }
    host.assign(hostStr.begin(), hostStr.end());
    return true;
}

bool DownloadManager::download(const std::string& url, const std::string& tmpPath) {
    m_cancelled = false;
    std::wstring host, path;
    int port = 80;
    if (!parseUrl(url, host, path, port)) { LOG_ERROR("invalid download URL: %s", url.c_str()); return false; }
    LOG_INFO("downloading: %s -> %s", url.c_str(), tmpPath.c_str());

    fs::path tmpFilePath(tmpPath);
    fs::create_directories(tmpFilePath.parent_path());

    HINTERNET hSession = WinHttpOpen(L"Launcher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { LOG_ERROR("WinHttpOpen failed"); return false; }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { LOG_ERROR("WinHttpConnect failed"); WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { LOG_ERROR("WinHttpOpenRequest failed"); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        LOG_ERROR("WinHttpSendRequest failed"); WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        LOG_ERROR("WinHttpReceiveResponse failed"); WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }

    // 检查 HTTP 状态码，非 200 视为下载失败（避免把服务端错误 JSON 当文件内容保存）
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
        if (statusCode != 200) {
            LOG_ERROR("下载失败，HTTP 状态码: %lu (url: %s)", statusCode, url.c_str());
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            return false;
        }
    }

    std::ofstream outFile(tmpPath, std::ios::binary);
    if (!outFile) { LOG_ERROR("cannot create tmp file: %s", tmpPath.c_str()); WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    DWORD dwSize = 0, dwDownloaded = 0;
    char buffer[8192];
    bool success = true;
    do {
        if (m_cancelled) { LOG_WARN("download cancelled"); success = false; break; }
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) { LOG_ERROR("WinHttpQueryDataAvailable failed"); success = false; break; }
        if (dwSize == 0) break;
        DWORD dwRead = 0, toRead = (dwSize < sizeof(buffer)) ? dwSize : sizeof(buffer);
        if (!WinHttpReadData(hRequest, buffer, toRead, &dwRead)) { LOG_ERROR("WinHttpReadData failed"); success = false; break; }
        outFile.write(buffer, dwRead); dwDownloaded += dwRead;
    } while (dwSize > 0);

    outFile.close();
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);

    if (success) LOG_INFO("download done: %lu bytes -> %s", dwDownloaded, tmpPath.c_str());
    else fs::remove(tmpPath);
    return success;
}

std::string DownloadManager::sha256(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) { LOG_ERROR("cannot open file for SHA256: %s", filePath.c_str()); return ""; }

    HCRYPTPROV hProv = 0; HCRYPTHASH hHash = 0;
    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) { CryptReleaseContext(hProv, 0); return ""; }

    char buf[8192];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0)
        CryptHashData(hHash, reinterpret_cast<BYTE*>(buf), static_cast<DWORD>(file.gcount()), 0);

    DWORD hashLen = 32; BYTE hashBytes[32]; char hashStr[65] = {};
    CryptGetHashParam(hHash, HP_HASHVAL, hashBytes, &hashLen, 0);
    for (DWORD i = 0; i < hashLen; ++i) sprintf_s(hashStr + i * 2, 3, "%02x", hashBytes[i]);
    CryptDestroyHash(hHash); CryptReleaseContext(hProv, 0);
    return hashStr;
}

void DownloadManager::cancel() { m_cancelled = true; }
