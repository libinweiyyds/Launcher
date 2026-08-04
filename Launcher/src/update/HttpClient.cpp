#include "HttpClient.h"
#include <Windows.h>
#include <curl/curl.h>
#include "../logger/Logger.h"

static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* body = static_cast<std::string*>(userp);
    size_t total = size * nmemb;
    body->append(static_cast<char*>(contents), total);
    return total;
}

HttpResponse HttpClient::get(const std::string& url) {
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) { LOG_ERROR("curl_easy_init failed"); return resp; }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) { curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.code); resp.ok = true; }
    else { LOG_ERROR("curl GET failed: %s", curl_easy_strerror(res)); }
    curl_easy_cleanup(curl);
    return resp;
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body) {
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) { LOG_ERROR("curl_easy_init failed"); return resp; }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) { curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.code); resp.ok = true; }
    else { LOG_ERROR("curl POST failed: %s", curl_easy_strerror(res)); }
    curl_easy_cleanup(curl);
    return resp;
}
