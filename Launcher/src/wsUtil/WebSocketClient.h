#pragma once
#pragma execution_character_set("utf-8")
#include <string>
#include <functional>
#include <memory>
#include "nlohmann/json.hpp"

struct lws_context;
struct lws;
struct lws_vhost;

/**
 * WebSocket client wrapper.
 *
 * Based on libwebsockets (C) + nlohmann-json (C++)
 * Connection: ws://localhost:8080/ws/{group}/{id}
 * Response:   {"code":200, "data"："", "desc"："...", "type"："param"}
 */
class WebSocketClient
{
public:
	enum class State {
		Disconnected,
		Connecting,
		Connected,
		Error
	};

	using MessageCallback = std::function<void(int code,
		const std::string& type,
		const std::string& desc,
		const std::string& data)>;
	using StateCallback = std::function<void(State state)>;

	WebSocketClient();
	~WebSocketClient();

	WebSocketClient(const WebSocketClient&) = delete;
	WebSocketClient& operator=(const WebSocketClient&) = delete;

	/**
	 * 连接ws 路径参数：ws://localhost:8080/ws/{group}/{id}
	 * group->分组     id->设备id
	 */
	void connect(const std::string& group, int id);

	/**
	 * 断开并清理ws连接
	 */
	void disconnect();

	/**
	 * 发送一条消息
	 */
	void send(const std::string& message);

	/**
	 * 是否连接成功
	 * @return  true->成功   false->未成功
	 */
	bool isConnected() const { return m_state == State::Connected; }

	/**
	 * 当前状态
	 */
	State state() const { return m_state; }

	/**
	* @brief 注册消息回调函数
	* @param cb 外部传入的消息处理回调对象
	*/
	void onMessage(MessageCallback cb) { m_msgCb = std::move(cb); }

	/**
	 * @brief 注册状态变更回调函数
	 * @param cb 外部传入的状态变化回调对象
	 */
	void onStateChange(StateCallback cb) { m_stateCb = std::move(cb); }

	/// 运行事件循环，直到连接断开或发生错误
	void run();

	/// 执行一次非阻塞轮询处理；返回 false 代表需要退出循环
	bool service();

	// lws 库静态回调接口 —— opaque_user_data 指针指向当前类实例对象
	/**
	 * @brief libwebsockets 库的全局静态回调入口
	 * @param wsi lws会话上下文结构体指针
	 * @param reason 本次触发回调的事件类型枚举值
	 * @param user 用户私有数据
	 * @param in 接收/发送的数据缓冲区
	 * @param len 数据缓冲区有效字节长度
	 * @return int lws要求的整型返回值
	 */
	static int lwsCallback(struct lws* wsi,
		enum lws_callback_reasons reason,
		void* user, void* in, size_t len);

private:
	void setState(State s);
	void handleMessage(const char* data, size_t len);

	lws_context* m_context = nullptr;
	lws_vhost* m_vhost = nullptr;
	lws* m_wsi = nullptr;

	std::string   m_address = "localhost";
	int           m_port = 8080;
	std::string   m_path;

	State         m_state = State::Disconnected;

	MessageCallback m_msgCb;
	StateCallback   m_stateCb;

	std::string m_pending_send;
};
