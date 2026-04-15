#pragma once
#include <jsoncpp/include/json/json.h>

namespace websocket
{
	// 设置日志函数，log_func参数是一个函数对象，接受一个字符串参数，表示要输出的日志文本
	void SetLogFunction(const std::function<void(const std::string& text)>& log_func);
	// 设置加密解密函数，encode参数是一个函数对象，接受一个字符串参数，表示要加密的文本，返回加密后的文本；decode参数是一个函数对象，接受一个字符串参数，表示要解密的文本，返回解密后的文本
	void SetCipherFunction(const std::function<std::string(const std::string& text)>& encode_func, const std::function<std::string(const std::string& text)>& decode_func);
	// WebSocket接口类，包含客户端和服务器共有的功能
	class ISocket
	{
	public:
		ISocket();
		void RegisterFunction(const std::string& name, const std::function<void(const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)>& func);
		void RegisterEvent(const std::string& name, const std::function<void(const Json::Value& param)>& proc);
	protected:
		void OnMessage(const std::string& message, const std::function<bool(const std::string& text)>& send);
	private:
		std::map<std::string, std::function<void(const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback)>> _functions;
		std::map<std::string, std::function<void(const Json::Value& param)>> _events;
	};
	// 客户端
	class Client :public ISocket
	{
	public:
		Client(const std::string& url, const std::function<Json::Value()>& getHelloData, const std::function<void(Client& client)>& onDisconnect);
		virtual ~Client();
		void Connect();
		void Disconnect();
		bool IsConnected();
		void CallFunction(const std::string& name, const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback);
		bool SendEvent(const std::string& name, const Json::Value& param);
	private:
		ix::WebSocket _socket;
		std::function<Json::Value()> _getHelloData;
		std::function<void(Client& client)> _onDisconnect;
	};
	// 服务端
	class Server :public ISocket
	{
	public:
		Server(unsigned short port, const std::function<std::string(const std::string& url)>& onConnect, const std::function<void(const std::string& clientId)>& onDisconnect);
		virtual ~Server();
		bool Start();
		void Stop();
		void DisconnectClient(const std::string& clientId);
		void CallFunction(const std::string& clientId, const std::string& name, const Json::Value& param, const std::function<void(bool success, const std::string& message, const Json::Value& data)>& callback);
		bool SendEvent(const std::string& clientId, const std::string& name, const Json::Value& param);
		void BroadcastEvent(const std::string& name, const Json::Value& param);
	private:
		ix::WebSocketServer _server;
		std::map<std::string, std::shared_ptr<ix::WebSocket>> _clients;
		std::mutex _clients_mutex;
		std::function<std::string(const std::string& url)> _onConnect;
		std::function<void(const std::string& clientId)> _onDisconnect;
	};
};

