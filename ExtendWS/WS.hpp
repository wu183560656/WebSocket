#pragma once
#include <Common/libWebSocket/websocket.h>
#include <Windows.h>

class WS
{
private:
	inline static constexpr const char* test_url = "ws://127.0.0.1:5555/Extend";
	inline static std::string g_platform;
	inline static std::string g_version;
	inline static std::function<Json::Value()> g_HelloDataCallback = nullptr;
	inline static websocket::Client* g_ws = nullptr;
	inline static websocket::Client* g_test_ws = nullptr;
public:
	// 初始化函数
	static bool Initialize(const std::string& url, const std::string& platform, const std::string& version, const std::function<Json::Value()>& HelloDataCallback)
	{
		if (g_ws || g_test_ws)
		{
			return false;
		}
		g_platform = platform;
		g_version = version;
		g_HelloDataCallback = HelloDataCallback;
		// 工作Socket
		g_ws = new websocket::Client(url, [HelloDataCallback]()->Json::Value {
			Json::Value result;
			result["platform"] = g_platform;
			result["version"] = g_version;
			result["pid"] = (int)GetCurrentProcessId();
			result["data"] = g_HelloDataCallback ? g_HelloDataCallback() : Json::nullValue;
			return result;
		}, [](websocket::Client& client) {
			// 重连
			client.Connect();
		});
		// 测试Socket，定时检查测试功能是否启用，如果启用则连接测试服务器
		g_test_ws = new websocket::Client(test_url, [HelloDataCallback]()->Json::Value {
			Json::Value result;
			result["platform"] = g_platform;
			result["version"] = g_version;
			result["pid"] = (int)GetCurrentProcessId();
			result["data"] = g_HelloDataCallback ? g_HelloDataCallback() : Json::nullValue;
			return result;
		}, nullptr);
		std::thread([]() {
			while (true)
			{
				if (!g_test_ws)
				{
					break;
				}
				else if (!g_test_ws->IsConnected())
				{
					HANDLE hMutext = OpenMutexW(SYNCHRONIZE, NULL, L"Global\\Extend_Test_Enable_Mutex");
					// 如果互斥体存在，说明测试功能被启用
					if (hMutext)
					{
						g_test_ws->Connect();
						CloseHandle(hMutext);
					}
				}
				std::this_thread::sleep_for(std::chrono::seconds(5));
			}
		}).detach();
		return true;
	}
	// 启动函数，连接服务器
	static void Connect()
	{
		if (g_ws && !g_ws->IsConnected())
		{
			g_ws->Connect();
		}
	}
	// 清理函数
	static void Uninitialize()
	{
		if (g_test_ws)
		{
			g_test_ws->Disconnect();
			delete g_test_ws;
			g_test_ws = nullptr;
		}
		if (g_ws)
		{
			g_ws->Disconnect();
			delete g_ws;
			g_ws = nullptr;
		}
	}
	static void RegisterFunction(const std::string& name, const std::function<void(const Json::Value&, const std::function<void(bool, std::string, Json::Value)>&)>& func)
	{
		if (g_ws)
		{
			g_ws->RegisterFunction(name, func);
		}
		if (g_test_ws)
		{
			g_test_ws->RegisterFunction(name, func);
		}
	}
	static void RegisterEvent(const std::string& name, const std::function<void(const Json::Value&)>& callback)
	{
		if (g_ws)
		{
			g_ws->RegisterEvent(name, callback);
		}
		if (g_test_ws)
		{
			g_test_ws->RegisterEvent(name, callback);
		}
	}
	static bool InvokeFunction(const std::string& name, const Json::Value& args, const std::function<void(bool, const std::string&, const Json::Value&)>& callback)
	{
		if (!g_ws)
		{
			return false;
		}
		g_ws->InvokeFunction(name, args, callback);
	}
	static void SendEvent(const std::string& name, const Json::Value& data)
	{
		if (g_ws)
		{
			g_ws->SendEvent(name, data);
		}
		if (g_test_ws)
		{
			g_test_ws->SendEvent(name, data);
		}
	}
};