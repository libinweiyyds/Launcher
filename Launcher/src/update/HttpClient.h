#pragma once

#include <string>

// HTTP 响应结构
struct HttpResponse {
    long code = 0;         // HTTP 状态码
    std::string body;      // 响应体
    bool ok = false;       // 请求是否成功（无网络/curl 错误）
};

// HTTP 客户端：libcurl 封装，只做请求，不关心业务逻辑
class HttpClient {
public:
    // GET 请求
    static HttpResponse get(const std::string& url);

    // POST 请求（body 为 JSON 字符串）
    static HttpResponse post(const std::string& url, const std::string& body);
};
