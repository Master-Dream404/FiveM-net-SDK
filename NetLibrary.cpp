#include "StdInc.h"
#include "NetLibrary.h"
#include "ICoreGameInit.h"
#include <mutex>
#include <mmsystem.h>
#include <IteratorView.h>
#include <optional>
#include <random>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <experimental/coroutine>
#include <pplawait.h>
#include <ppltasks.h>
#include <m_includes/json/json.hpp>
using json = nlohmann::json;

fwEvent<const std::string&> OnRichPresenceSetTemplate;
fwEvent<int, const std::string&> OnRichPresenceSetValue;

std::unique_ptr<NetLibraryImplBase> CreateNetLibraryImplV2(INetLibraryInherit* base);

#define TIMEOUT_DATA_SIZE 16

static uint32_t g_runFrameTicks[TIMEOUT_DATA_SIZE];
static uint32_t g_receiveDataTicks[TIMEOUT_DATA_SIZE];
static uint32_t g_sendDataTicks[TIMEOUT_DATA_SIZE];

static void AddTimeoutTick(uint32_t* timeoutList)
{
	memmove(&timeoutList[0], &timeoutList[1], (TIMEOUT_DATA_SIZE - 1) * sizeof(uint32_t));
	timeoutList[TIMEOUT_DATA_SIZE - 1] = timeGetTime();
}

void NetLibrary::AddReceiveTick()
{
	AddTimeoutTick(g_receiveDataTicks);
}

void NetLibrary::AddSendTick()
{
	AddTimeoutTick(g_sendDataTicks);
}

static uint32_t m_tempGuid = GetTickCount();

uint16_t NetLibrary::GetServerNetID()
{
	return m_serverNetID;
}

uint16_t NetLibrary::GetServerSlotID()
{
	return m_serverSlotID;
}

uint16_t NetLibrary::GetHostNetID()
{
	return m_hostNetID;
}

void NetLibrary::HandleConnected(int serverNetID, int hostNetID, int hostBase, int slotID, uint64_t serverTime)
{
	m_serverNetID = serverNetID;
	m_hostNetID = hostNetID;
	m_hostBase = hostBase;
	m_serverSlotID = slotID;
	m_serverTime = serverTime;

	m_reconnectAttempts = 0;
	m_lastReconnect = 0;

	OnConnectOKReceived(m_currentServer);

	if (m_connectionState != CS_ACTIVE)
	{
		m_connectionState = CS_CONNECTED;
	}
	else
	{
		Instance<ICoreGameInit>::Get()->ClearVariable("networkTimedOut");
	}
}

bool NetLibrary::GetOutgoingPacket(RoutingPacket& packet)
{
	return m_outgoingPackets.try_pop(packet);
}

bool NetLibrary::WaitForRoutedPacket(uint32_t timeout)
{
	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		if (!m_incomingPackets.empty())
		{
			return true;
		}
	}

	WaitForSingleObject(m_receiveEvent, timeout);

	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		return (!m_incomingPackets.empty());
	}
}


void NetLibrary::EnqueueRoutedPacket(uint16_t netID, const std::string& packet)
{
	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		RoutingPacket routePacket;
		routePacket.netID = netID;
		routePacket.payload = std::move(packet);
		routePacket.genTime = timeGetTime();

		m_incomingPackets.push(std::move(routePacket));
	}

	SetEvent(m_receiveEvent);
}

bool NetLibrary::DequeueRoutedPacket(char* buffer, size_t* length, uint16_t* netID)
{
	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		if (m_incomingPackets.empty())
		{
			return false;
		}

		auto packet = m_incomingPackets.front();
		m_incomingPackets.pop();

		memcpy(buffer, packet.payload.c_str(), packet.payload.size());
		*netID = packet.netID;
		*length = packet.payload.size();

		// store metrics
		auto timeval = (timeGetTime() - packet.genTime);

		m_metricSink->OnRouteDelayResult(timeval);
	}

	ResetEvent(m_receiveEvent);

	return true;
}

void NetLibrary::RoutePacket(const char* buffer, size_t length, uint16_t netID)
{
	RoutingPacket routePacket;
	routePacket.netID = netID;
	routePacket.payload = std::string(buffer, length);

	m_outgoingPackets.push(routePacket);
}

#define	BIG_INFO_STRING		8192  // used for system info key only
#define	BIG_INFO_KEY		  8192
#define	BIG_INFO_VALUE		8192

const char* Info_ValueForKey(const char* s, const char* key)
{
	char	pkey[BIG_INFO_KEY];
	static	char value[2][BIG_INFO_VALUE];	// use two buffers so compares
											// work without stomping on each other
	static	int	valueindex = 0;
	char* o;

	if (!s || !key)
	{
		return "";
	}

	if (strlen(s) >= BIG_INFO_STRING)
	{
		return "";
	}

	valueindex ^= 1;
	if (*s == '\\')
		s++;
	while (1)
	{
		o = pkey;
		while (*s != '\\')
		{
			if (!*s)
				return "";
			*o++ = *s++;
		}
		*o = 0;
		s++;

		o = value[valueindex];

		while (*s != '\\' && *s)
		{
			*o++ = *s++;
		}
		*o = 0;

		if (!_stricmp(key, pkey))
			return value[valueindex];

		if (!*s)
			break;
		s++;
	}

	return "";
}


#define Q_IsColorString( p )  ( ( p ) && *( p ) == '^' && *( ( p ) + 1 ) && isdigit( *( ( p ) + 1 ) ) ) // ^[0-9]

void StripColors(const char* in, char* out, int max)
{
	max--; // \0
	int current = 0;
	while (*in != 0 && current < max)
	{
		if (!Q_IsColorString(in))
		{
			*out = *in;
			out++;
			current++;
		}
		else
		{
			*in++;
		}
		*in++;
	}
	*out = '\0';
}

void NetLibrary::ProcessOOB(const NetAddress& from, const char* oob, size_t length)
{

}

void NetLibrary::SetHost(uint16_t netID, uint32_t base)
{
	m_hostNetID = netID;
	m_hostBase = base;
}

void NetLibrary::SetBase(uint32_t base)
{
	m_serverBase = base;
}

uint32_t NetLibrary::GetHostBase()
{
	return m_hostBase;
}

void NetLibrary::SetMetricSink(fwRefContainer<INetMetricSink>& sink)
{
	m_metricSink = sink;
}

void NetLibrary::HandleReliableCommand(uint32_t msgType, const char* buf, size_t length)
{
	//auto range = m_reliableHandlers.equal_range( msgType );

	//for ( auto& handlerPair : fx::GetIteratorView( range ) )
	//{
	//	auto [handler , runOnMainFrame] = handlerPair.second;

	//	if ( runOnMainFrame )
	//	{
	//		auto server = m_currentServerPeer;
	//		net::Buffer netBuf( reinterpret_cast<const uint8_t*>( buf ) , length );

	//		m_mainFrameQueue.push( [ this , netBuf , handler , server ] ( )
	//			{
	//				if ( server != m_currentServerPeer )
	//				{
	//					//trace( "Ignored a network packet enqueued before reconnection.\n" );
	//					return;
	//				}

	//				handler( reinterpret_cast<const char*>( netBuf.GetBuffer( ) ) , netBuf.GetLength( ) );
	//			} );
	//	}
	//	else
	//	{
	//		handler( buf , length );
	//	}
	//}
}

inline constexpr uint32_t HashRageString(const char* string)
{
	uint32_t hash = 0;

	for (; *string; ++string)
	{
		hash += *string;
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash;
}

RoutingPacket::RoutingPacket()
{
	//genTime = timeGetTime();
	genTime = 0;
}

void NetLibrary::SendReliableCommand(const char* type, const char* buffer, size_t length)
{
	if (m_impl)
	{
		m_impl->SendReliableCommand(HashRageString(type), buffer, length);
	}
}

void NetLibrary::SendUnreliableCommand(const char* type, const char* buffer, size_t length)
{
	if (m_impl)
	{
		m_impl->SendUnreliableCommand(HashRageString(type), buffer, length);
	}
}

static std::string g_disconnectReason;

static std::mutex g_netFrameMutex;

inline uint64_t GetGUID()
{
	return (uint64_t)(0x210000100000000 | m_tempGuid);
}

uint64_t NetLibrary::GetGUID()
{
	return ::GetGUID();
}

void NetLibrary::RunMainFrame()
{
	std::function<void()> cb;

	while (m_mainFrameQueue.try_pop(cb))
	{
		cb();
	}
}

void NetLibrary::RunFrame()
{

}

void NetLibrary::Death()
{
	g_netFrameMutex.unlock();
}

void NetLibrary::Resurrection()
{
	g_netFrameMutex.lock();
}

void NetLibrary::OnConnectionError(const std::string& errorString, const std::string& metaData /* = "{}" */)
{
	OnConnectionErrorEvent(errorString.c_str());
	OnConnectionErrorRichEvent(errorString, metaData);
}

// hack for NetLibraryImplV2
int g_serverVersion;

concurrency::task<void> NetLibrary::ConnectToServer(const std::string& rootUrl)
{

}

void NetLibrary::SubmitCardResponse(const std::string& dataJson, const std::string& token)
{
	auto responseHandler = m_cardResponseHandler;

	if (responseHandler)
	{
		responseHandler(dataJson, token);
	}
}

void NetLibrary::CancelDeferredConnection()
{
	if (m_handshakeRequest)
	{
		m_handshakeRequest->Abort();
		m_handshakeRequest = {};
	}

	if (m_connectionState == CS_INITING)
	{
		m_connectionState = CS_IDLE;
	}
}

static std::mutex g_disconnectionMutex;

void NetLibrary::Disconnect(const char* reason)
{
	g_disconnectReason = reason;

	OnAttemptDisconnect(reason);
	//GameInit::KillNetwork((const wchar_t*)1);

	std::unique_lock<std::mutex> lock(g_disconnectionMutex);

	if (m_connectionState == CS_DOWNLOADING)
	{
		OnFinalizeDisconnect(m_currentServer);
	}

	if (m_connectionState == CS_CONNECTING || m_connectionState == CS_ACTIVE || m_connectionState == CS_FETCHING)
	{
		SendReliableCommand("msgIQuit", g_disconnectReason.c_str(), g_disconnectReason.length() + 1);

		if (m_impl)
		{
			m_impl->Flush();
			m_impl->Reset();
		}

		OnFinalizeDisconnect(m_currentServer);

		m_connectionState = CS_IDLE;
		m_currentServer = NetAddress();
	}
}

void NetLibrary::CreateResources()
{
	m_httpClient = Instance<HttpClient>::Get();
}

void NetLibrary::SendOutOfBand(const NetAddress& address, const char* format, ...)
{
	static char buffer[32768];

	*(int*)buffer = -1;

	va_list ap;
	va_start(ap, format);
	int length = _vsnprintf(&buffer[4], 32764, format, ap);
	va_end(ap);

	if (length >= 32764)
	{

	}

	buffer[32767] = '\0';

	SendData(address, buffer, strlen(buffer));
}

bool NetLibrary::IsPendingInGameReconnect()
{
	return (m_connectionState == CS_ACTIVE && m_impl->IsDisconnected());
}

const char* NetLibrary::GetPlayerName()
{
	/*
	auto function = reinterpret_cast<const char* (__fastcall*)(void*)>(mem::get_pattern(("net.dll"), ("48 83 EC ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 ? 48 83 B9 ? ? ? ? ?")));
	return function(reinterpret_cast<void*>(this));
	*/
}

void NetLibrary::SetPlayerName(const char* name)
{
	m_playerName = name;
}

void NetLibrary::SendData(const NetAddress& address, const char* data, size_t length)
{
	m_impl->SendData(address, data, length);
}

void NetLibrary::AddReliableHandler(const char* type, const ReliableHandlerType& function, bool runOnMainThreadOnly /* = false */)
{
	uint32_t hash = HashRageString(type);

	m_reliableHandlers.insert({ hash, { function, runOnMainThreadOnly } });
}

void NetLibrary::DownloadsComplete()
{
	if (m_connectionState == CS_DOWNLOADING)
	{
		m_connectionState = CS_DOWNLOADCOMPLETE;
	}
}

bool NetLibrary::ProcessPreGameTick()
{
	if (m_connectionState != CS_ACTIVE && m_connectionState != CS_CONNECTED && m_connectionState != CS_IDLE)
	{
		RunFrame();

		return false;
	}

	return true;
}

void NetLibrary::SendNetEvent(const std::string& eventName, const std::string& jsonString, int i)
{
	const char* cmdType = "msgNetEvent";

	if (i == -1)
	{
		i = UINT16_MAX;
	}
	else if (i == -2)
	{
		cmdType = "msgServerEvent";
	}

	size_t eventNameLength = eventName.length();

	net::Buffer buffer;

	if (i >= 0)
	{
		buffer.Write<uint16_t>(i);
	}

	buffer.Write<uint16_t>(eventNameLength + 1);
	buffer.Write(eventName.c_str(), eventNameLength + 1);

	buffer.Write(jsonString.c_str(), jsonString.size());

	SendReliableCommand(cmdType, reinterpret_cast<const char*>(buffer.GetBuffer()), buffer.GetCurOffset());
}

NetLibrary::NetLibrary()
	: m_serverNetID(0), m_serverBase(0), m_hostBase(0), m_hostNetID(0), m_connectionState(CS_IDLE), m_lastConnectionState(CS_IDLE),
	m_lastConnect(0), m_impl(nullptr)

{
	m_receiveEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

fwEvent<NetLibrary*> NetLibrary::OnNetLibraryCreate;
//fwEvent<const std::function<void( uint32_t , const char* , int )>&> NetLibrary::OnBuildMessage;

NetLibrary* NetLibrary::Create()
{
	auto lib = new NetLibrary();

	lib->CreateResources();

	lib->AddReliableHandler("msgIHost", [=](const char* buf, size_t len)
		{
			net::Buffer buffer(reinterpret_cast<const uint8_t*>(buf), len);

			uint16_t hostNetID = buffer.Read<uint16_t>();
			uint32_t hostBase = buffer.Read<uint32_t>();

			lib->SetHost(hostNetID, hostBase);
		});

	OnNetLibraryCreate(lib);

	return lib;
}

int32_t NetLibrary::GetPing()
{
	if (m_impl)
	{
		return m_impl->GetPing();
	}

	return -1;
}

int32_t NetLibrary::GetVariance()
{
	if (m_impl)
	{
		return m_impl->GetVariance();
	}

	return -1;
}

void NetLibrary::SetRichError(const std::string& data /* = "{}" */)
{
	m_richError = data;
}