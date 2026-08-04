#include "WebSocketClient.h"

#include <iostream>
#include <sstream>
#include <cstring>

#include <libwebsockets.h>
#include "WsDebug.h"

static const struct lws_protocols s_protocols[] = {
    {
        "wsdemo-client",
        WebSocketClient::lwsCallback,
        0,                         // per_session_data_size
        0,                         // rx buffer size (0 = default)
    },
    { nullptr, nullptr, 0, 0 }
};

WebSocketClient::WebSocketClient()
{
}

WebSocketClient::~WebSocketClient()
{
    disconnect();
}

void WebSocketClient::connect(const std::string& group, int id)
{
    if (m_state != State::Disconnected) {
        std::cerr << "[WebSocketClient] 已经连接或者连接中......"
                  << std::endl;
        return;
    }

    // 构建连接请求url: /ws/{group}/{id}
    std::ostringstream oss;
    oss << "/ws/" << group << "/" << id;
    m_path = oss.str();

    setState(State::Connecting);

	// 创建 lws 上下文（仅客户端模式，不开启服务监听端口）
	lws_context_creation_info info{};
	info.port = CONTEXT_PORT_NO_LISTEN;       // 标识该上下文不绑定监听端口，纯客户端使用
	info.protocols = s_protocols;                  // 绑定预先定义的协议数组配置
	info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT; // 开启全局SSL环境初始化选项
	info.user = this;                         // 将当前类实例指针存入用户私有数据域，回调中可取出使用

    m_context = lws_create_context(&info);
    if (!m_context) {
        std::cerr << "[WebSocketClient] 创建 lws 上下文失败."
                  << std::endl;
        setState(State::Error);
        return;
    }

    // 设置连接参数
    lws_client_connect_info param{};
    param.context          = m_context;
    param.address          = m_address.c_str();
    param.port             = m_port;
    param.path             = m_path.c_str();
    param.host             = m_address.c_str();
    param.origin           = m_address.c_str();
    param.protocol         = s_protocols[0].name;
    param.ssl_connection   = 0;
    param.opaque_user_data = this;

    m_wsi = lws_client_connect_via_info(&param);
    if (!m_wsi) {
        std::cerr << "[WebSocketClient] lws_client_connect_via_info failed."
                  << std::endl;
        lws_context_destroy(m_context);
        m_context = nullptr;
        setState(State::Error);
    }
}

void WebSocketClient::disconnect()
{
    if (m_context) {
        lws_context_destroy(m_context);
        m_context = nullptr;
    }
    m_wsi = nullptr;
    m_pending_send.clear();
    setState(State::Disconnected);
}

void WebSocketClient::send(const std::string& message)
{
    if (!m_wsi || m_state != State::Connected) {
        std::cerr << "[WebSocketClient] Cannot send: not connected."
                  << std::endl;
        return;
    }

    m_pending_send = message;
    lws_callback_on_writable(m_wsi);
}

void WebSocketClient::run()
{
    while (m_context &&
           m_state != State::Disconnected &&
           m_state != State::Error) {
        lws_service(m_context, 0);
    }
}

bool WebSocketClient::service()
{
    if (!m_context)
        return false;
    return lws_service(m_context, 10) >= 0;
}

void WebSocketClient::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    if (m_stateCb)
        m_stateCb(m_state);
}


void WebSocketClient::handleMessage(const char* data, size_t len)
{
    std::string raw(data, len);
    wsDebug("收到原始数据: " + raw);

    try {
        auto j = nlohmann::json::parse(raw);

        int    code    = j.value("code", 0);
        std::string type   = j.value("type", "");
        std::string desc   = j.value("desc", "");
        std::string msgData = j.value("data", "");

        if (m_msgCb)
            m_msgCb(code, type, desc, msgData);

    } catch (const nlohmann::json::parse_error&) {
        wsDebug("JSON parse error");
    } catch (const std::exception&) {
        wsDebug("Message handling error");
    }
}


int WebSocketClient::lwsCallback(struct lws* wsi,
                                  enum lws_callback_reasons reason,
                                  void* /*user*/, void* in, size_t len)
{
    auto* self = static_cast<WebSocketClient*>(
        lws_get_opaque_user_data(wsi));

    if (!self && reason != LWS_CALLBACK_CLIENT_CONNECTION_ERROR) {
        return -1;
    }

    switch (reason) {

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        std::cerr << "[WebSocketClient] Connection error: "
                  << (in ? static_cast<const char*>(in) : "unknown")
                  << std::endl;
        if (self)
            self->setState(State::Error);
        return -1;

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        std::cout << "[WebSocketClient] 连接到 "
                  << self->m_address << "：" << self->m_port << self->m_path
                  << std::endl;
        self->setState(State::Connected);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        self->handleMessage(static_cast<const char*>(in), len);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (!self->m_pending_send.empty()) {
            const auto& msg = self->m_pending_send;
            size_t payloadLen = msg.size();

            unsigned char buf[LWS_PRE + 4096];
            if (payloadLen > 4096)
                payloadLen = 4096;

            std::memcpy(&buf[LWS_PRE], msg.data(), payloadLen);

            int n = lws_write(wsi, &buf[LWS_PRE], payloadLen, LWS_WRITE_TEXT);
            if (n < static_cast<int>(payloadLen)) {
                std::cerr << "[WebSocketClient] 写入数据 (" << n << ")"
                          << std::endl;
                return -1;
            }
            self->m_pending_send.clear();
        }
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        std::cout << "[WebSocketClient] 退出连接" << std::endl;
        self->setState(State::Disconnected);
        return -1;

    case LWS_CALLBACK_WSI_DESTROY:
        break;

    default:
        break;
    }

    return 0;
}
