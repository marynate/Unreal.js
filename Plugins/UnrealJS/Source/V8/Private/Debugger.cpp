#include "V8PCH.h"
#include "JavascriptIsolate.h"
#include "JavascriptContext.h"
#include "SocketSubSystem.h"
#include "Sockets.h"
#include "NetworkMessage.h"
#include "Misc/DefaultValueHelper.h"

#if PLATFORM_WINDOWS
#include "AllowWindowsPlatformTypes.h"
#endif
#include <thread>
#if PLATFORM_WINDOWS
#include "HideWindowsPlatformTypes.h"
#endif

#include "Translator.h"

using namespace v8;

#if WITH_EDITOR
#include "TickableEditorObject.h"

namespace 
{
	//@HACK : to be multi-threaded and support multiple sockets
	FSocket* MainSocket{ nullptr };

	void RegisterDebuggerSocket(FSocket* Socket)
	{
		MainSocket = Socket;
	}

	void UnregisterDebuggerSocket(FSocket* Socket)
	{
		MainSocket = nullptr;
	}

	void SendTo(FSocket* Socket_, const char* msg)
	{
		FSimpleAbstractSocket_FSocket Socket(Socket_);

		auto send = [&](const char* msg) {
			Socket.Send(reinterpret_cast<const uint8*>(msg), strlen(msg));
		};

		send(TCHAR_TO_UTF8(*FString::Printf(TEXT("Content-Length: %d\r\n"), strlen(msg))));
		send("\r\n");
		send(msg);

		//UE_LOG(Javascript, Log, TEXT("Reply : %s"), UTF8_TO_TCHAR(msg));
	}

	void Broadcast(const char* msg)
	{
		if (MainSocket)
		{
			SendTo(MainSocket,msg);
		}

		//UE_LOG(Javascript, Log, TEXT("Broadcast : %s"), UTF8_TO_TCHAR(msg));
	}
}

class FDebugger : public IJavascriptDebugger, public FTickableEditorObject
{
public:
	struct FClientData : public Debug::ClientData
	{
		FClientData(FDebugger* InDebugger, FSocket* InSocket)
		: Debugger(InDebugger), Socket_(InSocket)
		{
			Debugger->Busy.Increment();
		}

		~FClientData()
		{
			Debugger->Busy.Decrement();
		}

		FDebugger* Debugger;
		FSocket* Socket_;

		void Send(const char* msg)
		{
			SendTo(Socket_, msg);
		}
	};

	std::thread thread;
	Isolate* isolate_;
	Persistent<Context> context_;

	Local<Context> context() { return Local<Context>::New(isolate_, context_); }

	virtual void Tick(float DeltaTime) override
	{
		Isolate::Scope isolate_scope(isolate_);
		HandleScope handle_scope(isolate_);
		Context::Scope debug_scope(context());

		Debug::ProcessDebugMessages();
	}

	virtual bool IsTickable() const override
	{
		return true;
	}

	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FV8Debugger, STATGROUP_Tickables);
	}	

	FDebugger(int32 InPort, Local<Context> context)
	{
		isolate_ = context->GetIsolate();
		context_.Reset(isolate_,context);

		StopRequested.Reset();

		Install();

		thread = std::thread([this, InPort]{Main(InPort); });				
	}

	void Install()
	{
		Debug::SetMessageHandler([](const Debug::Message& message){
			FClientData* data = static_cast<FClientData*>(message.GetClientData());
			auto json = message.GetJSON();

			//UE_LOG(Javascript, Log, TEXT("V8 message : %s %s"), message.IsEvent() ? TEXT("(event)") : TEXT(""), message.IsResponse() ? TEXT("(response)") : TEXT("") );			

			if (json.IsEmpty())
			{
				UE_LOG(Javascript, Error, TEXT("Not a json"));
			}
			else
			{
				String::Utf8Value utf8(json);
				auto str = *utf8;

				if (data)
				{
					data->Send(str);
				}
				else
				{
					Broadcast(str);
				}
			}
		});
	}

	void Uninstall()
	{		
		Context::Scope context_scope(context());
		Debug::SetMessageHandler(nullptr);
	}

	~FDebugger()
	{
		Uninstall();

		context_.Reset();
	}

	virtual void Destroy() override
	{
		Stop();		

		delete this;
	}

	void Stop()
	{
		StopRequested.Set(true);

		thread.join();
	}	
	
	bool Main(int32 Port)
	{
		auto SocketSubsystem = ISocketSubsystem::Get();
		if (!SocketSubsystem) return ReportError("No socket subsystem");

		auto Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("V8 debugger"));
		if (!Socket) return ReportError("Failed to create socket");

		auto Cleanup_Socket = [&]{Socket->Close(); SocketSubsystem->DestroySocket(Socket); };
		
		// listen on any IP address
		auto ListenAddr = SocketSubsystem->GetLocalBindAddr(*GLog);

		ListenAddr->SetPort(Port);
		Socket->SetReuseAddr();

		// bind to the address
		if (!Socket->Bind(*ListenAddr)) return ReportError("Failed to bind socket", Cleanup_Socket );

		if (!Socket->Listen(16)) return ReportError("Failed to listen", Cleanup_Socket );
				
		int32 port = Socket->GetPortNo();
		check((Port == 0 && port != 0) || port == Port);

		ListenAddr->SetPort(port);		

		while (!StopRequested.GetValue())
		{
			bool bReadReady = false;

			// check for incoming connections
			if (Socket->HasPendingConnection(bReadReady) && bReadReady)
			{
				auto ClientSocket = Socket->Accept(TEXT("Remote V8 connection"));

				if (!ClientSocket) continue;				

				RegisterDebuggerSocket(ClientSocket);

				UE_LOG(Javascript, Log, TEXT("V8 Debugger session start"));

				Logic(ClientSocket);

				UE_LOG(Javascript, Log, TEXT("V8 Debugger session stop"));

				while (Busy.GetValue() != 0)
				{
					FPlatformProcess::Sleep(0.25f);
				}

				UnregisterDebuggerSocket(ClientSocket);
				
				ClientSocket->Close();
				SocketSubsystem->DestroySocket(ClientSocket);
			}
			else
			{
				FPlatformProcess::Sleep(0.25f);
			}		
		}

		Cleanup_Socket();		

		return true;
	}

	bool Logic(FSocket* InSocket)
	{
		FSimpleAbstractSocket_FSocket Socket(InSocket);

		auto send = [&](const char* msg) {
			Socket.Send(reinterpret_cast<const uint8*>(msg), strlen(msg));
		};

		auto read_line = [&](FString& line) {
			const auto buffer_size = 1024;
			ANSICHAR buffer[buffer_size];
			for (int pos = 0; pos < buffer_size; pos++)			
			{
				char ch;
				if (!Socket.Receive(reinterpret_cast<uint8*>(&ch), 1)) return false;

				if (pos && buffer[pos - 1] == '\r' && ch == '\n')
				{
					buffer[pos - 1] = '\0';
					line = UTF8_TO_TCHAR(buffer);
					return true;
				}
				else if (ch == '\n')
				{
					buffer[pos] = '\0';
					line = UTF8_TO_TCHAR(buffer);
					return true;
				}
				buffer[pos] = ch;				
			}
			return false;
		};

		auto command = [&](const FString& request) {			
			Debug::SendCommand(isolate_, (uint16_t*)*request, request.Len(), new FClientData(this,InSocket));
		};

		auto process = [&](const FString& content) {
			//UE_LOG(Javascript, Log, TEXT("Received: %s"), *content);
			command(content);
			return content.Find(TEXT("\"type\":\"request\",\"command\":\"disconnect\"}")) == INDEX_NONE;
		};

		auto bye = [&] {
			command(TEXT("{\"seq\":1,\"type:\":\"request\",\"command\":\"disconnect\"}"));
		};

		send("Type: connect\r\n");
		send("V8-Version: ");
		send(v8::V8::GetVersion());
		send("\r\n");
		send("Protocol-Version: 1\r\n");
		send("Embedding-Host: UnrealEngine\r\n");
		send("Content-Length: 0\r\n");
		send("\r\n");		

		FString json_text;
		int32 content_length = -1;

		for (;;)
		{
			FString line, left, right;
			if (!read_line(line)) break;			

			if (line.Split(TEXT(":"), &left, &right))
			{				
				if (left == TEXT("Content-Length"))
				{
					if (!FDefaultValueHelper::ParseInt(right, content_length)) return ReportError("invalid content length");
				}
			}
			else
			{	
				if (content_length <= 0) return ReportError("invalid content length < 0");

				char* buffer = new char[content_length+1];
				if (!buffer) break;

				if (!Socket.Receive(reinterpret_cast<uint8*>(buffer), content_length)) break;

				buffer[content_length] = '\0';
				if (!process(UTF8_TO_TCHAR(buffer))) return ReportError("failed to process");				
			}			
		}

		bye();

		return true;
	}	

	bool ReportError(const char* Error)
	{
		UE_LOG(Javascript, Error, TEXT("%s"), UTF8_TO_TCHAR(Error));

		// always false
		return false;
	}

	template <typename Fn>
	bool ReportError(const char* Error, Fn&& fn)
	{		
		fn();
		return ReportError(Error);
	}

	FThreadSafeCounter StopRequested;	
	FThreadSafeCounter Busy;
};

IJavascriptDebugger* IJavascriptDebugger::Create(int32 InPort,Local<Context> InContext)
{
	return new FDebugger(InPort,InContext);
}
#else
IJavascriptDebugger* IJavascriptDebugger::Create(int32 InPort, Local<Context> InContext)
{
	return nullptr;
} 
#endif