#include "ConfigSync.h"
#include "../logger/Logger.h"
#include "DownloadManager.h"
#include <SimpleIni.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>

// SimpleIni 可能间接引入 Windows 宏，与 LogLevel::ERROR 等冲突
#ifdef ERROR
#undef ERROR
#endif
#ifdef max
#undef max
#endif

namespace fs = std::filesystem;

// 通用工具：把 string 路径追加到 wstring
static std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// 通用工具：读文件全部字节
static std::string readFileAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// 递归复制目录（旧版本 config 文件夹 → 新版本目录）
void ConfigSync::copyDirRecursive(const std::wstring& src, const std::wstring& dst) {
    fs::path srcP(src), dstP(dst);
    fs::create_directories(dstP);
    for (const auto& entry : fs::recursive_directory_iterator(srcP)) {
        fs::path rel = fs::relative(entry.path(), srcP);
        fs::path target = dstP / rel;
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else {
            fs::create_directories(target.parent_path());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
}

// JSON 深度合并：旧值优先、新增字段追加
Json::Value ConfigSync::deepMergeJson(const Json::Value& oldV, const Json::Value& newV) {
    Json::Value result = oldV;  // 旧值优先
    if (!newV.isObject()) return result;
    for (const auto& key : newV.getMemberNames()) {
        if (oldV.isMember(key)) {
            // 两者都是 object → 递归合并
            if (oldV[key].isObject() && newV[key].isObject()) {
                result[key] = deepMergeJson(oldV[key], newV[key]);
            } else {
                // 类型不同或非 object：保留旧值
                result[key] = oldV[key];
            }
        } else {
            // 新版本新增字段
            result[key] = newV[key];
        }
    }
    return result;
}

// 把 remote.content 写入 baseDir/localFilePath（保留为最简兜底）
bool ConfigSync::syncConfig(const RemoteConfig& remote, const std::wstring& baseDir) {
    if (!remote.valid) {
        LOG_ERROR("ConfigSync: remote config invalid");
        return false;
    }
    std::wstring wideRel(remote.localFilePath.begin(), remote.localFilePath.end());
    fs::path target = fs::path(baseDir) / wideRel;
    LOG_INFO("syncConfig: target=%ls, content长度=%zu, sha256=%s",
        target.wstring().c_str(), remote.content.size(), remote.sha256.c_str());
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        LOG_ERROR("ConfigSync: cannot create parent dir: %ls (%s)",
            target.parent_path().wstring().c_str(), ec.message().c_str());
        return false;
    }
    if (fs::exists(target)) {
        std::string localHash = DownloadManager::sha256(target.string());
        if (localHash == remote.sha256) {
            LOG_INFO("ConfigSync: local file up-to-date, skip: %ls", target.wstring().c_str());
            return true;
        }
    }
    if (remote.content.empty()) {
        LOG_ERROR("ConfigSync: remote.content 为空，不写入空文件");
        return false;
    }
    {
        FILE* outFile = nullptr;
        if (_wfopen_s(&outFile, target.wstring().c_str(), L"wb") != 0 || outFile == nullptr) {
            LOG_ERROR("ConfigSync: cannot write file: %ls", target.wstring().c_str());
            return false;
        }
        fwrite(remote.content.data(), 1, remote.content.size(), outFile);
        fclose(outFile);
    }
    std::string newHash = DownloadManager::sha256(target.string());
    if (newHash != remote.sha256) {
        LOG_ERROR("ConfigSync: sha256 verify failed after write: exp=%s, got=%s",
            remote.sha256.c_str(), newHash.c_str());
        std::filesystem::remove(target, ec);
        return false;
    }
    LOG_INFO("ConfigSync: wrote %llu bytes to %ls (sha256 OK)",
        static_cast<unsigned long long>(remote.content.size()), target.wstring().c_str());
    return true;
}

// 版本升级配置继承 + 合并
// 行为：
//   1. baseDir/{configDir} 已存在 → 不做任何事，返回 true（hash 一致则视为已同步）
//   2. baseDir/{configDir} 不存在：
//      a. 遍历 history 找第一个有 config 目录的旧版本
//      b. 找到 → 复制整个 config 目录 + JSON 合并
//      c. 找不到 → 当作全新安装，直接写入 remote.content，调用 onFirstInstall
bool ConfigSync::inheritAndMergeConfig(
    const RemoteConfig& remote,
    const std::wstring& baseDir,
    const std::vector<std::string>& history,
    const std::wstring& versionsRoot,
    const std::string& currentVersion,
    const std::function<void(const std::string&)>& onFirstInstall) {

    if (!remote.valid) {
        LOG_ERROR("ConfigSync::inheritAndMergeConfig: remote invalid");
        return false;
    }

    // 1) 解析 config 目录相对路径（remote.localFilePath 的父目录）
    std::wstring wideRel(remote.localFilePath.begin(), remote.localFilePath.end());
    fs::path cfgFileRel(wideRel);                  // config/config.json
    fs::path cfgDirRel = cfgFileRel.parent_path(); // config

    fs::path baseP(baseDir);
    fs::path newCfgDir = baseP / cfgDirRel;
    fs::path target = baseP / cfgFileRel;

    // 2) 本地配置文件存在 → 本地优先，跳过任何同步操作
    //    设计依据：启动期不覆盖本地配置。
    //      - 退出上传失败时，远程版本可能低于本地 → 不能用远程覆盖本地
    //      - 本地配置由 baseline sha256 + 退出上传机制保证一致性，不需要启动期再校验
    //    注：跳过判据是文件存在，不是目录存在——目录存在但文件被删时仍要走同步逻辑
    if (fs::exists(target) && !fs::is_directory(target)) {
        LOG_INFO("ConfigSync: 本地配置文件存在，本地优先，跳过: %ls", target.wstring().c_str());
        return true;
    }

    // 3) 遍历 history 找第一个有 config 目录的旧版本
    std::wstring oldCfgDir;
    for (const auto& oldVer : history) {
        if (oldVer == currentVersion) continue;  // 跳过当前版本
        fs::path candidate = fs::path(versionsRoot) / widen(oldVer) / cfgDirRel;
        if (fs::exists(candidate) && fs::is_directory(candidate)) {
            oldCfgDir = candidate.wstring();
            LOG_INFO("ConfigSync: 找到旧版本配置目录: %s -> %ls",
                oldVer.c_str(), oldCfgDir.c_str());
            break;
        }
    }

    if (oldCfgDir.empty()) {
        // 全新安装：直接写入 remote.content
        LOG_INFO("ConfigSync: 未找到旧版本配置目录，按全新安装处理");
        if (!syncConfig(remote, baseDir)) return false;
        if (onFirstInstall) {
            std::string hash = DownloadManager::sha256(target.string());
            onFirstInstall(hash);
        }
        return true;
    }

    // 4) 复制旧版本 config 目录到新版本目录
    copyDirRecursive(oldCfgDir, newCfgDir.wstring());

    // 5) 根据 localFilePath 后缀判断格式，执行对应合并
    bool isIni = remote.localFilePath.size() >= 4
        && remote.localFilePath.compare(
            remote.localFilePath.length() - 4, 4,
            ".ini") == 0;

    if (isIni) {
        // INI 格式合并：使用 SimpleIniA，用户值优先（旧 key 存在则保留）
        LOG_INFO("ConfigSync: 检测到 INI 格式，使用 SimpleIniA 合并");
        CSimpleIniA ini;
        // 将目标路径从 wstring 转为 UTF-8 narrow string
        int narrowLen = WideCharToMultiByte(CP_UTF8, 0,
            target.wstring().c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string narrowPath(narrowLen - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0,
            target.wstring().c_str(), -1, &narrowPath[0], narrowLen, nullptr, nullptr);

        SI_Error rc = ini.LoadFile(narrowPath.c_str());
        if (rc != SI_OK) {
            LOG_WARN("ConfigSync: 旧 INI 文件加载失败 (rc=%d)，将创建新文件", rc);
        }

        // 解析 remote.content 为 INI 并合并（用户值优先）
        CSimpleIniA iniNew;
        iniNew.LoadData(remote.content.c_str());

        // 获取新配置所有 section，遍历其 key
        // TNamesDepend 是 CSimpleIniTempl 的嵌套类型，需用完整模板名访问
        using TND = CSimpleIniTempl<char, SI_NoCase<char>, SI_ConvertA<char>>::TNamesDepend;
        TND sections;
        iniNew.GetAllSections(sections);
        for (const auto& secEntry : sections) {
            const char* secName = secEntry.pItem;
            if (!secName || !secName[0]) continue;

            TND keys;
            iniNew.GetAllKeys(secName, keys);
            for (const auto& keyEntry : keys) {
                const char* keyName = keyEntry.pItem;
                const char* newVal = iniNew.GetValue(secName, keyName, nullptr);
                if (!keyName || !newVal) continue;

                // 旧值存在则保留（用户优先）
                const char* oldVal = ini.GetValue(secName, keyName, nullptr);
                if (oldVal == nullptr) {
                    ini.SetValue(secName, keyName, newVal);
                    LOG_INFO("ConfigSync: INI 新增 [%s] %s=%s", secName, keyName, newVal);
                } else {
                    LOG_INFO("ConfigSync: INI 保留旧值 [%s] %s=%s (忽略新值=%s)",
                        secName, keyName, oldVal, newVal);
                }
            }
        }

        // 写回 INI 文件
        rc = ini.SaveFile(narrowPath.c_str());
        if (rc != SI_OK) {
            LOG_ERROR("ConfigSync: INI 文件保存失败 (rc=%d)", rc);
            return false;
        }
        LOG_INFO("ConfigSync: INI 合并完成");
    } else {
        // JSON 格式合并：旧 config.json + remote.content → 写回新版本
        Json::Value oldRoot;
        Json::Value newRoot;
        if (fs::exists(target)) {
            std::string oldContent = readFileAll(target);
            Json::CharReaderBuilder b;
            std::string err;
            std::istringstream ss(oldContent);
            if (!Json::parseFromStream(b, ss, &oldRoot, &err)) {
                LOG_WARN("ConfigSync: 旧 config.json 解析失败，视为空对象: %s", err.c_str());
                oldRoot = Json::Value(Json::objectValue);
            }
        }
        {
            Json::CharReaderBuilder b;
            std::string err;
            std::istringstream ss(remote.content);
            if (!Json::parseFromStream(b, ss, &newRoot, &err)) {
                LOG_WARN("ConfigSync: remote.content 解析失败，使用空对象合并: %s", err.c_str());
                newRoot = Json::Value(Json::objectValue);
            }
        }

        Json::Value merged = deepMergeJson(oldRoot, newRoot);

        // 写回新版本 config.json（二进制 + UTF-8）
        Json::StreamWriterBuilder wbuilder;
        wbuilder["indentation"] = "    ";
        wbuilder["emitUTF8"] = true;
        std::string outJson = Json::writeString(wbuilder, merged);
        {
            FILE* outFile = nullptr;
            if (_wfopen_s(&outFile, target.wstring().c_str(), L"wb") != 0 || outFile == nullptr) {
                LOG_ERROR("ConfigSync: cannot write merged config: %ls", target.wstring().c_str());
                return false;
            }
            fwrite(outJson.data(), 1, outJson.size(), outFile);
            fclose(outFile);
        }
        LOG_INFO("ConfigSync: JSON 合并完成");
    }

    std::string newHash = DownloadManager::sha256(target.string());
    LOG_INFO("ConfigSync: 合并完成 hash=%s...", newHash.substr(0, 8).c_str());
    return true;
}

// 退出时：检测变化并上传
// 不同 → POST 上传当前内容；上传成功 → 调用 onUploaded 回调
// 上传失败（3 次重试）→ 返回 false，baselineSha256 保持不变（自动重试）
bool ConfigSync::uploadConfigIfChanged(
    const std::string& host, int port,
    const std::string& configId,
    const std::string& hostName,
    const std::string& baseDir,
    const std::string& configRelPath,
    const std::string& baselineSha256,
    const std::function<void(const std::string&)>& onUploaded) {

    fs::path baseP = fs::path(std::wstring(baseDir.begin(), baseDir.end()));
    fs::path target = baseP / fs::path(std::wstring(configRelPath.begin(), configRelPath.end()));

    if (!fs::exists(target)) {
        LOG_WARN("ConfigSync::uploadConfigIfChanged: config 文件不存在，跳过: %ls",
            target.wstring().c_str());
        return false;
    }

    std::string currentHash = DownloadManager::sha256(target.string());
    if (currentHash == baselineSha256) {
        LOG_INFO("ConfigSync: 配置未变更（sha256 一致），跳过上传");
        return true;
    }

    // 读当前文件内容
    std::string content = readFileAll(target);

    // 构造上传 body（按文档约定：{softwareConfigId, hostName, configContent}）
    Json::Value body;
    body["softwareConfigId"] = configId;
    body["hostName"] = hostName;
    body["configContent"] = content;

    Json::StreamWriterBuilder wbuilder;
    wbuilder["indentation"] = "";
    wbuilder["emitUTF8"] = true;
    std::string jsonBody = Json::writeString(wbuilder, body);

    // 重试 3 次
    const int kMaxRetry = 3;
    for (int i = 0; i < kMaxRetry; ++i) {
        LOG_INFO("ConfigSync: 上传配置变更 (attempt %d/%d)", i + 1, kMaxRetry);
        if (UpdateUtils::uploadClientConfig(host, port, jsonBody)) {
            // 调用回调（更新启动器 config.json 的 software.sha256）
            if (onUploaded) onUploaded(currentHash);
            LOG_INFO("ConfigSync: 上传成功，sha256 已更新");
            return true;
        }
    }
    LOG_WARN("ConfigSync: 上传失败 %d 次，sha256 保持不变，下次启动将重试", kMaxRetry);
    return false;
}