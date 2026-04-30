#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <Common/jsoncpp/include/json/json.h>
#include <semaphore>
#include <combaseapi.h>
#include "websocket.h"
#pragma comment(lib, "ws2_32.lib")

namespace websocket
{
	// 待处理调用超时时间，单位毫秒
	static constexpr unsigned __int64 PendingCallTimeout = 30000;
	// 日志函数，默认不输出日志，可以通过调用SetLogFunction设置日志函数
	static std::function<void(const std::string& text)> g_log_func = [](const std::string& text) {};
	void SetLogFunction(const std::function<void(const std::string& text)>& log_func)
	{
		g_log_func = log_func;
	}
	// 加密解密函数，默认不加密不解密，可以通过调用SetCipherFunction设置加密解密函数
	static std::function<std::string(const std::string& text)> g_encode_func = [](const std::string& text) { return text; };
	static std::function<std::string(const std::string& text)> g_decode_func = [](const std::string& text) { return text; };
	void SetCipherFunction(const std::function<std::string(const std::string& text)>& encode_func, const std::function<std::string(const std::string& text)>& decode_func)
	{
		g_encode_func = encode_func;
		g_decode_func = decode_func;
	}
	static std::string JsonToString(const Json::Value& value)
	{
		Json::StreamWriterBuilder builder;
		builder["indentation"] = "";
		builder["emitUTF8"] = true;
		std::string json_str = Json::writeString(builder, value);
		std::string cipher_str = g_encode_func(json_str);
		return cipher_str;
	}
	static Json::Value StringToJson(const std::string& text)
	{
		std::string decipher_str = g_decode_func(text);
		Json::Value root;
		Json::CharReaderBuilder builder;
		std::string errs;
		std::istringstream iss(decipher_str);
		if (!Json::parseFromStream(builder, iss, &root, &errs))
		{
			g_log_func("Failed to parse message: " + errs);
		}
		return root;
	}

	ISocket::PendingCall::PendingCall(const std::function<void(bool success, const std::string& message, const Json::Value& data)> callback/* = nullptr*/)
		: _timeout(GetTickCount64() + PendingCallTimeout), _callback(callback)
	{
		return;
	}
	unsigned __int64 ISocket::PendingCall::GetTimeout() const
	{
		return _timeout;
	}
	std::function<void(bool success, const std::string& message, const Json::Value& data)> ISocket::PendingCall::GetCallback() const
	{
		return _callback;
	}

	ISocket::ISocket()
		: _stop_pending_calls_cleaner_thread(false)
	{
		// 是否已初始化网络系统，网络系统只需要初始化一次，可以通过调用ISocket的构造函数自动初始化
		static bool g_initialized = false;
		if (!g_initialized)
		{
			g_initialized = true;
			ix::initNetSystem();
		}
		// 启动一个后台线程定期清理过期的待处理调用，避免调用超时
		_pending_calls_cleaner_thread = new std::thread([this]() {
			while (!_stop_pending_calls_cleaner_thread)
			{
				auto current_time = GetTickCount64();
				while (true)
				{
					// 先取出来再调用，避免在调用回调函数时持有锁，导致死锁
					std::function<void(bool success, const std::string& message, const Json::Value& data)> callback = nullptr;
					{
						std::lock_guard<std::mutex> lock(_pending_calls_mutex);
						for (auto iter = _pending_calls.begin(); iter != _pending_calls.end(); ++iter)
						{
							if (current_time >= iter->second.GetTimeout())
							{
								callback = iter->second.GetCallback();
								_pending_calls.erase(iter);
								break;
							}
						}
					}
					if (!callback)
					{
						break;
					}
					callback(false, (const char*)u8"调用超时", Json::Value());
				}
				std::this_thread::sleep_for(std::chrono::seconds(1));
				// 调用其他处理函数，子类可以重写这个函数来处理一些特殊的消息，比如ping/pong等
				OtherHandleProc();
			}
			// 线程退出前清理所有待处理调用
			while (true)
			{
				std::function<void(bool success, const std::string& message, const Json::Value& data)> callback = nullptr;
				{
					std::lock_guard<std::mutex> lock(_pending_calls_mutex);
					if (_pending_calls.begin() != _pending_calls.end())
					{
						callback = _pending_calls.begin()->second.GetCallback();
						_pending_calls.erase(_pending_calls.begin());
					}
				}
				if (!callback)
				{
					break;
				}
				callback(false, (const char*)u8"调用超时", Json::Value());
			}
		});
	}
	ISocket::~ISocket()
	{
		// 停止后台线程并等待线程结束，避免线程访问已销毁的对象
		_stop_pending_calls_cleaner_thread = true;
		_pending_calls_cleaner_thread->join();
		delete _pending_calls_cleaner_thread;
	}

	std::string ISocket::AppendPendingCall(const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)
	{
		std::string id = "00000000-0000-0000-0000-000000000000";
		GUID guid = { 0 };
		if (SUCCEEDED(CoCreateGuid(&guid)))
		{
			char buf[64] = { 0 };
			sprintf_s(buf, sizeof(buf), "%08X-%04X-%04x-%02X%02X-%02X%02X%02X%02X%02X%02X", guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
			id = std::string(buf);
		}
		{
			std::lock_guard<std::mutex> lock(_pending_calls_mutex);
			_pending_calls[id] = PendingCall(callback);
		}
		return id;
	}
	void ISocket::InvokePendingCall(const std::string& id, bool success, const std::string& message, const Json::Value& data)
	{
		// 先取出来再调用，避免在调用回调函数时持有锁，导致死锁
		std::function<void(bool success, const std::string& message, const Json::Value& data)> callback = nullptr;
		{
			std::lock_guard<std::mutex> lock(_pending_calls_mutex);
			auto iter = _pending_calls.find(id);
			if (iter != _pending_calls.end())
			{
				callback = iter->second.GetCallback();
				_pending_calls.erase(iter);
			}
		}
		if (callback)
		{
			callback(success, message, data);
		}
	}
	void ISocket::OtherHandleProc()
	{
		return;
	}

	Client::Client(const std::string& url, const std::function<Json::Value()>& getHelloData, const std::function<void(Client& client)>& onDisconnect)
		: _getHelloData(getHelloData), _onDisconnect(onDisconnect)
		, _socket(new ix::WebSocket()), _last_received_message_time(0), _last_send_ping_time(0)
		, _undefinedFunctionHandler(nullptr), _undefinedEventHandler(nullptr)
	{
		_socket->setUrl(url);
		_socket->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
			switch (msg->type)
			{
			case ix::WebSocketMessageType::Open:
			{
				// 连接建立成功，记录当前时间
				_last_received_message_time = GetTickCount64();
				// 连接建立成功，发送hello事件
				Json::Value hello_data = Json::nullValue;
				if(this->_getHelloData)
				{
					hello_data = this->_getHelloData();
				}
				Json::Value root;
				Json::Value body;
				root["type"] = "event";
				body["name"] = "hello";
				body["param"] = hello_data;
				root["body"] = body;
				this->_socket->sendUtf8Text(JsonToString(root));
			}
			break;
			case ix::WebSocketMessageType::Message:
			{
				g_log_func("Received message: " + msg->str);
				// 收到消息，更新最后收到消息的时间
				this->_last_received_message_time = GetTickCount64();
				this->_last_send_ping_time = GetTickCount64();
				Json::Value root = StringToJson(msg->str);
				if (root.isNull())
				{
					g_log_func("Message is null");
					return;
				}
				std::string type = root["type"].asString();
				if(type == "ping")
				{
					// 收到ping消息，发送pong响应
					this->_socket->sendUtf8Text(R"({"type":"pong"})");
					return;
				}
				if (type == "pong")
				{
					// 心跳响应，不处理
					return;
				}

				// 收到消息，根据消息类型处理事件或函数调用
				Json::Value body = root["body"];
				if (body.isNull())
				{
					g_log_func("Message body is null");
					return;
				}
				// 根据消息类型处理事件或函数调用
				if (type == "event")
				{
					// 触发事件回调
					std::string name = body["name"].asString();
					auto iter = _events.find(name);
					if (iter != _events.end())
					{
						iter->second(body["param"]);
					}
					return;
				}
				else if (type == "invoke")
				{
					// 调用注册的函数并发送响应
					std::string name = body["name"].asString();
					std::string id = body["id"].asString();
					auto callback = [name, id, this](bool success, const std::string& message, const Json::Value& data) {
						Json::Value response_body;
						response_body["id"] = id;
						response_body["success"] = success;
						response_body["message"] = message;
						response_body["data"] = data;
						Json::Value response_root;
						response_root["type"] = "response";
						response_root["body"] = response_body;
						std::string send_msg = JsonToString(response_root);
						g_log_func("Sending response for function " + name + ": " + send_msg);
						this->_socket->sendUtf8Text(send_msg);
					};
					auto iter = _functions.find(name);
					if (iter != _functions.end())
					{
						iter->second(body["param"], callback);
					}
					else if (!_undefinedFunctionHandler)
					{
						callback(false, (const char*)u8"函数不存在", Json::Value());
					}
					else
					{
						_undefinedFunctionHandler(name, body["param"], callback);
					}
					return;
				}
				else if (type == "response")
				{
					// 处理函数调用的响应
					std::string id = body["id"].asString();
					bool success = body["success"].asBool();
					std::string message = body["message"].asString();
					Json::Value data = body["data"];
					// 通过id找到对应的回调函数并调用
					InvokePendingCall(id, success, message, data);
					return;
				}
				else
				{
					g_log_func("Unknown message type: " + type);
					return;
				}
			}
			break;
			case ix::WebSocketMessageType::Close:
			case ix::WebSocketMessageType::Error:
			{
				if (this->_onDisconnect)
				{
					this->_onDisconnect(*this);
				}
			}
			break;
			default:
				break;
			}
		});
	}
	Client::~Client()
	{
		Disconnect();
		delete _socket;
	}
	void Client::RegisterFunction(const std::string& name, const std::function<void(const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)>& func)
	{
		_functions[name] = func;
	}
	void Client::RegisterEvent(const std::string& name, const std::function<void(const Json::Value& param)>& proc)
	{
		_events[name] = proc;
	}
	void Client::SetUndefinedFunctionHandler(const std::function<void(const std::string& name, const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)>& handler)
	{
		_undefinedFunctionHandler = handler;
	}
	void Client::SetUndefinedEventHandler(const std::function<void(const std::string& name, const Json::Value& param)>& handler)
	{
		_undefinedEventHandler = handler;
	}
	void Client::Connect()
	{
		if (IsConnected())
			return;
		_socket->start();
	}
	void Client::Disconnect()
	{
		_socket->stop();
	}
	bool Client::IsConnected()
	{
		return _socket->getReadyState() == ix::ReadyState::Open;
	}
	void Client::InvokeFunction(const std::string& name, const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)
	{
		if (!IsConnected())
		{
			callback(false, (const char*)u8"未连接", Json::Value());
			return;
		}
		Json::Value root;
		root["type"] = "invoke";
		Json::Value body;
		body["name"] = name;
		std::string id = AppendPendingCall(callback);
		body["id"] = id;
		body["param"] = param;
		root["body"] = body;
		if (!_socket->sendUtf8Text(JsonToString(root)).success)
		{
			InvokePendingCall(id, false, (const char*)u8"请求发送失败", Json::Value());
			return;
		}
		return;
	}
	bool Client::SendEvent(const std::string& name, const Json::Value& param)
	{
		if (!IsConnected())
			return false;
		Json::Value root;
		root["type"] = "event";
		Json::Value body;
		body["name"] = name;
		body["param"] = param;
		root["body"] = body;
		std::string msg = JsonToString(root);
		g_log_func("Sending event: " + msg);
		return _socket->sendUtf8Text(msg).success;
	}
	void Client::OtherHandleProc()
	{
		auto tc = GetTickCount64();
		if (IsConnected() && tc - _last_send_ping_time > 30000)
		{
			// 如果超过30秒没有发送ping消息，主动发送ping消息
			_socket->sendUtf8Text(R"({"type":"ping"})");
			_last_send_ping_time = tc;
		}
		if (tc - _last_received_message_time > 60000)
		{
			// 如果一分钟没收到消息了，说明可能连接已经断开了，主动断开连接
			g_log_func("No message received for a long time, disconnecting...");
			Disconnect();
		}
	}
	Server::Server(unsigned short port, const std::function<std::string(const std::string& url)>& onConnect, const std::function<void(const std::string& clientId)>& onDisconnect)
		: _onConnect(onConnect), _onDisconnect(onDisconnect)
		, _server(new ix::WebSocketServer(port))
	{
		_server->setOnConnectionCallback([this](std::weak_ptr<ix::WebSocket> webSocket, std::shared_ptr<ix::ConnectionState> connectionState) {
			auto client = webSocket.lock();
			if (!client)
				return;
			// 在连接回调中获取客户端ID
			std::string clientId = _onConnect(client->getUrl());
			if (clientId.empty())
			{
				// 如果ID为空，说明不接受该连接，直接关闭连接并返回
				client->close();
				return;
			}
			// 将客户端添加到客户端列表中
			{
				std::lock_guard<std::mutex> lock(this->_clients_mutex);
				this->_clients[clientId] = client;
			}
			// 设置消息回调和断开连接回调
			client->setOnMessageCallback([this, clientId, client](const ix::WebSocketMessagePtr& msg) {
				switch (msg->type)
				{
				case ix::WebSocketMessageType::Message:
				{
					g_log_func("Received message: " + msg->str);
					Json::Value root = StringToJson(msg->str);
					if (root.isNull())
					{
						g_log_func("Message is null");
						return;
					}
					std::string type = root["type"].asString();
					if (type == "ping")
					{
						// 收到ping消息后直接回复pong消息，无需触发事件回调
						client->sendUtf8Text(R"({"type":"pong"})");
						return;
					}
					if (type == "pong")
					{
						// 心跳响应，不处理
						return;
					}
					// 收到消息，根据消息类型处理事件或函数调用
					Json::Value body = root["body"];
					if (body.isNull())
					{
						g_log_func("Message body is null");
						return;
					}
					// 根据消息类型处理事件或函数调用
					if (type == "event")
					{
						// 触发事件回调
						std::string name = body["name"].asString();
						auto iter = _events.find(name);
						if (iter != _events.end())
						{
							iter->second(clientId, body["param"]);
						}
						return;
					}
					else if (type == "invoke")
					{
						// 调用注册的函数并发送响应
						std::string name = body["name"].asString();
						std::string id = body["id"].asString();
						auto callback = [name, id, client](bool success, const std::string& message, const Json::Value& data) {
							Json::Value response_body;
							response_body["id"] = id;
							response_body["success"] = success;
							response_body["message"] = message;
							response_body["data"] = data;
							Json::Value response_root;
							response_root["type"] = "response";
							response_root["body"] = response_body;
							std::string send_msg = JsonToString(response_root);
							g_log_func("Sending response for function " + name + ": " + send_msg);
							client->sendUtf8Text(send_msg);
						};
						auto iter = _functions.find(name);
						if (iter != _functions.end())
						{
							iter->second(clientId, body["param"], callback);
						}
						else if (!_undefinedFunctionHandler)
						{
							callback(false, (const char*)u8"函数不存在", Json::Value());
						}
						else
						{
							_undefinedFunctionHandler(name, clientId, body["param"], callback);
						}
						return;
					}
					else if (type == "response")
					{
						// 处理函数调用的响应
						std::string id = body["id"].asString();
						bool success = body["success"].asBool();
						std::string message = body["message"].asString();
						Json::Value data = body["data"];
						// 通过id找到对应的回调函数并调用
						InvokePendingCall(id, success, message, data);
						return;
					}
					else
					{
						g_log_func("Unknown message type: " + type);
						return;
					}
				}
					break;
				case ix::WebSocketMessageType::Close:
				case ix::WebSocketMessageType::Error:
				{
					client->close();
					if (this->_onDisconnect)
					{
						this->_onDisconnect(clientId);
					}
					{
						std::lock_guard<std::mutex> lock(this->_clients_mutex);
						this->_clients.erase(clientId);
					}
				}
					break;
				default:
					break;
				}
			});
		});
	}
	Server::~Server()
	{
		Stop();
		delete _server;;
	}
	void Server::RegisterFunction(const std::string& name, const std::function<void(const std::string& clientId, const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)>& func)
	{
		_functions[name] = func;
	}
	void Server::RegisterEvent(const std::string& name, const std::function<void(const std::string& clientId, const Json::Value& param)>& proc)
	{
		_events[name] = proc;
	}
	void Server::SetUndefinedFunctionHandler(const std::function<void(const std::string& name, const std::string& clientId, const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)>& handler)
	{
		_undefinedFunctionHandler = handler;
	}
	void Server::SetUndefinedEventHandler(const std::function<void(const std::string& name, const std::string& clientId, const Json::Value& param)>& handler)
	{
		_undefinedEventHandler = handler;
	}
	bool Server::Start()
	{
		return _server->listenAndStart();
	}
	void Server::Stop()
	{
		_server->stop();
	}
	int Server::GetPort()
	{
		return _server->getPort();
	}
	void Server::DisconnectClient(const std::string& clientId)
	{
		std::shared_ptr<ix::WebSocket> client;
		{
			std::lock_guard<std::mutex> lock(this->_clients_mutex);
			auto iter = this->_clients.find(clientId);
			if (iter != this->_clients.end())
			{
				client = iter->second;
				this->_clients.erase(iter);
			}
		}
		if (client)
		{
			client->close();
		}
	}
	void Server::InvokeFunction(const std::string& clientId, const std::string& name, const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)
	{
		std::shared_ptr<ix::WebSocket> client;
		{
			std::lock_guard<std::mutex> lock(this->_clients_mutex);
			auto iter = this->_clients.find(clientId);
			if (iter != this->_clients.end())
			{
				client = iter->second;
			}
		}
		if (!client)
		{
			callback(false, (const char*)u8"客户端不存在", Json::Value());
			return;
		}
		Json::Value root;
		root["type"] = "invoke";
		Json::Value body;
		body["name"] = name;
		std::string id = AppendPendingCall(callback);
		body["id"] = id;
		body["param"] = param;
		root["body"] = body;
		if (!client->sendUtf8Text(JsonToString(root)).success)
		{
			InvokePendingCall(id, false, (const char*)u8"请求发送失败", Json::Value());
			return;
		}
	}
	bool Server::SendEvent(const std::string& clientId, const std::string& name, const Json::Value& param)
	{
		std::shared_ptr<ix::WebSocket> client;
		{
			std::lock_guard<std::mutex> lock(this->_clients_mutex);
			auto iter = this->_clients.find(clientId);
			if (iter != this->_clients.end())
			{
				client = iter->second;
			}
		}
		if (!client)
		{
			return false;
		}
		Json::Value root;
		root["type"] = "event";
		Json::Value body;
		body["name"] = name;
		body["param"] = param;
		root["body"] = body;
		return client->sendUtf8Text(JsonToString(root)).success;
	}
	void Server::BroadcastEvent(const std::string& name, const Json::Value& param)
	{
		Json::Value root;
		root["type"] = "event";
		Json::Value body;
		body["name"] = name;
		body["param"] = param;
		root["body"] = body;
		std::string msg = JsonToString(root);
		{
			std::lock_guard<std::mutex> lock(this->_clients_mutex);
			for (auto& pair : this->_clients)
			{
				pair.second->sendUtf8Text(msg);
			}
		}
	}
};