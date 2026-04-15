#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <semaphore>
#include <Windows.h>
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
	class PendingCall
	{
	public:
		PendingCall(const std::function<void(bool success, const std::string& message, const Json::Value& data)> callback = nullptr)
			: _timeout(GetTickCount64() + PendingCallTimeout), _callback(callback)
		{
			return;
		}
		std::function<void(bool success, const std::string& message, const Json::Value& data)> GetCallback() const
		{
			return _callback;
		}
		static std::string Append(const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)
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
				std::lock_guard<std::mutex> lock(g_pending_calls_mutex);
				g_pending_calls[id] = PendingCall(callback);
			}
			return id;
		}
		static void Invoke(const std::string& id, bool success, const std::string& message, const Json::Value& data)
		{
			// 先取出来再调用，避免在调用回调函数时持有锁，导致死锁
			std::function<void(bool success, const std::string& message, const Json::Value& data)> callback = nullptr;
			{
				std::lock_guard<std::mutex> lock(g_pending_calls_mutex);
				auto iter = g_pending_calls.find(id);
				if (iter != g_pending_calls.end())
				{
					callback = iter->second.GetCallback();
					g_pending_calls.erase(iter);
				}
			}
			if (callback)
			{
				callback(success, message, data);
			}
		}
		static void CleanExpired()
		{
			auto current_time = GetTickCount64();
			while (true)
			{
				// 先取出来再调用，避免在调用回调函数时持有锁，导致死锁
				std::function<void(bool success, const std::string& message, const Json::Value& data)> callback;
				{
					std::lock_guard<std::mutex> lock(g_pending_calls_mutex);
					for (auto iter = g_pending_calls.begin(); iter != g_pending_calls.end(); ++iter)
					{
						if (current_time >= iter->second._timeout)
						{
							callback = iter->second.GetCallback();
							g_pending_calls.erase(iter);
							break;
						}
					}
				}
				if(!callback)
				{
					break;
				}
				callback(false, (const char*)u8"调用超时", Json::Value());
			}
		}
	private:
		unsigned __int64 _timeout;
		std::function<void(bool success, const std::string& message, const Json::Value& data)> _callback;
		// 全局待处理调用列表
		inline static std::map<std::string, PendingCall> g_pending_calls;
		inline static std::mutex g_pending_calls_mutex;
	};

	ISocket::ISocket()
	{
		// 是否已初始化网络系统，网络系统只需要初始化一次，可以通过调用ISocket的构造函数自动初始化
		static bool g_initialized = false;
		if (!g_initialized)
		{
			ix::initNetSystem();
			std::thread([]() {
				while (true)
				{
					// 定期清理过期的待处理调用，避免内存泄漏
					PendingCall::CleanExpired();
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}).detach();
			g_initialized = true;
		}
	}
	void ISocket::RegisterFunction(const std::string& name, const std::function<void(const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)>& func)
	{
		_functions[name] = func;
	}
	void ISocket::RegisterEvent(const std::string& name, const std::function<void(const Json::Value& param)>& proc)
	{
		_events[name] = proc;
	}
	void ISocket::OnMessage(const std::string& message, const std::function<bool(const std::string& text)>& send)
	{
		Json::Value root = StringToJson(message);
		if(root.isNull())
		{
			g_log_func("Message is null");
			return;
		}
		std::string type = root["type"].asString();
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
			auto iter = _functions.find(name);
			if (iter == _functions.end())
			{
				Json::Value response_body;
				response_body["id"] = id;
				response_body["success"] = false;
				response_body["message"] = (const char*)u8"函数不存在";
				Json::Value response_root;
				response_root["type"] = "response";
				response_root["body"] = response_body;
				std::string send_msg = JsonToString(response_root);
				send(send_msg);
			}
			else
			{
				iter->second(body["param"], [name, id, send](bool success, const std::string& message, const Json::Value& data) {
					Json::Value response_body;
					response_body["id"] = id;
					response_body["success"] = success;
					response_body["message"] = message;
					response_body["data"] = data;
					Json::Value response_root;
					response_root["type"] = "response";
					response_root["body"] = response_body;
					std::string send_msg = JsonToString(response_root);
					send(send_msg);
				});
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
			PendingCall::Invoke(id, success, message, data);
			return;
		}
		else
		{
			g_log_func("Unknown message type: " + type);
			return;
		}
	}

	Client::Client(const std::string& url, const std::function<Json::Value()>& getHelloData, const std::function<void(Client& client)>& onDisconnect)
		: _getHelloData(getHelloData), _onDisconnect(onDisconnect)
		, _socket()
	{
		_socket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
			switch (msg->type)
			{
			case ix::WebSocketMessageType::Open:
			{
				Json::Value hello_data = Json::nullValue;
				if(this->_getHelloData)
				{
					hello_data = this->_getHelloData();
				}
				Json::Value root;
				root["type"] = "hello";
				root["body"] = hello_data;
				this->_socket.sendUtf8Text(JsonToString(root));
			}
			break;
			case ix::WebSocketMessageType::Message:
			{
				this->OnMessage(msg->str, [this](const std::string& response_msg) {
					return this->_socket.sendUtf8Text(response_msg).success;
				});
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
	}
	void Client::Connect()
	{
		if (IsConnected())
			return;
		_socket.start();
	}
	void Client::Disconnect()
	{
		_socket.stop();
	}
	bool Client::IsConnected()
	{
		return _socket.getReadyState() == ix::ReadyState::Open;
	}
	void Client::CallFunction(const std::string& name, const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)
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
		std::string id = PendingCall::Append(callback);
		body["id"] = id;
		body["param"] = param;
		root["body"] = body;
		if (!_socket.sendUtf8Text(JsonToString(root)).success)
		{
			PendingCall::Invoke(id, false, (const char*)u8"请求发送失败", Json::Value());
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
		return _socket.sendUtf8Text(JsonToString(root)).success;
	}

	Server::Server(unsigned short port, const std::function<std::string(const std::string& url)>& onConnect, const std::function<void(const std::string& clientId)>& onDisconnect)
		: _onConnect(onConnect), _onDisconnect(onDisconnect)
		, _server(port)
	{
		_server.setOnConnectionCallback([this](std::weak_ptr<ix::WebSocket> webSocket, std::shared_ptr<ix::ConnectionState> connectionState) {
			auto client = webSocket.lock();
			if (!client)
				return;
			// 在连接回调中获取客户端ID
			std::string clientId = _onConnect(client->getUrl());
			if (clientId.empty())
			{
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
					this->OnMessage(msg->str, [client](const std::string& response_msg) {
						return client->send(response_msg).success;
					});
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
	}
	bool Server::Start()
	{
		return _server.listenAndStart();
	}
	void Server::Stop()
	{
		_server.stop();
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
	void Server::CallFunction(const std::string& clientId, const std::string& name, const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)
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
		std::string id = PendingCall::Append(callback);
		body["id"] = id;
		body["param"] = param;
		root["body"] = body;
		if (!client->sendUtf8Text(JsonToString(root)).success)
		{
			PendingCall::Invoke(id, false, (const char*)u8"请求发送失败", Json::Value());
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