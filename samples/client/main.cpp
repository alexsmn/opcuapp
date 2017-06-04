#include <iostream>
#include <string>
#include <thread>

#include "opcuapp/proxy_stub.h"
#include "opcuapp/client/session.h"
#include "opcuapp/client/subscription.h"
#include "opcuapp/client/monitored_item.h"

using namespace std::chrono_literals;

namespace {

OpcUa_ProxyStubConfiguration MakeProxyStubConfiguration() {
  OpcUa_ProxyStubConfiguration result = {};
  result.bProxyStub_Trace_Enabled = OpcUa_True;
  result.uProxyStub_Trace_Level = OPCUA_TRACE_OUTPUT_LEVEL_WARNING;
  result.iSerializer_MaxAlloc = -1;
  result.iSerializer_MaxStringLength = -1;
  result.iSerializer_MaxByteStringLength = -1;
  result.iSerializer_MaxArrayLength = -1;
  result.iSerializer_MaxMessageSize = -1;
  result.iSerializer_MaxRecursionDepth = -1;
  result.bSecureListener_ThreadPool_Enabled = OpcUa_False;
  result.iSecureListener_ThreadPool_MinThreads = -1;
  result.iSecureListener_ThreadPool_MaxThreads = -1;
  result.iSecureListener_ThreadPool_MaxJobs = -1;
  result.bSecureListener_ThreadPool_BlockOnAdd = OpcUa_True;
  result.uSecureListener_ThreadPool_Timeout = OPCUA_INFINITE;
  result.bTcpListener_ClientThreadsEnabled = OpcUa_False;
  result.iTcpListener_DefaultChunkSize = -1;
  result.iTcpConnection_DefaultChunkSize = -1;
  result.iTcpTransport_MaxMessageLength = -1;
  result.iTcpTransport_MaxChunkCount = -1;
  result.bTcpStream_ExpectWriteToBlock = OpcUa_True;
  return result;
}

} // namespace

/*class Client {
 public:
  void Connect(const opcua::String& url);

  opcua::client::Subscription& subscription() { return subscription_; }

 private:
  void CreateSession();
  void ActivateSession();

  void CreateSubscription();

  void OnError();

  opcua::Platform platform;
  opcua::ProxyStub proxy_stub_{platform, MakeProxyStubConfiguration()};

  opcua::client::Channel channel_{OpcUa_Channel_SerializerType_Binary};
  opcua::client::Session session_{channel_};
  opcua::client::Subscription subscription_{session_, {}};

  bool session_created_ = false;
  bool session_activated_ = false;
};

void Client::Connect(const opcua::String& url) {
  opcua::ByteString client_private_key;

  OpcUa_P_OpenSSL_CertificateStore_Config pki_config{
      OpcUa_NO_PKI,
  };

  opcua::String requested_security_policy_uri{OpcUa_SecurityPolicy_None};

  opcua::client::ChannelContext context{
      const_cast<OpcUa_StringA>(url.raw_string()),
      nullptr,
      client_private_key.pass(),
      nullptr,
      &pki_config,
      requested_security_policy_uri.pass(),
      0,
      OpcUa_MessageSecurityMode_None,
      10000,
  };

  channel_.Connect(context, [this](opcua::StatusCode status_code, OpcUa_Channel_Event event) {
    if (event != eOpcUa_Channel_Event_Connected)
      return OnError();
    if (!session_created_)
      CreateSession();
  });
}

void Client::CreateSession() {
  assert(!session_created_);

  opcua::CreateSessionRequest request;
  request.ClientDescription.ApplicationType = OpcUa_ApplicationType_Client;
	OpcUa_String_AttachReadOnly(&request.ClientDescription.ApplicationName.Text, "TestClientName");
	OpcUa_String_AttachReadOnly(&request.ClientDescription.ApplicationUri, "TestClientUri");
	OpcUa_String_AttachReadOnly(&request.ClientDescription.ProductUri, "TestProductUri");

  session_.Create(request, [this](opcua::StatusCode status_code) {
    if (!status_code)
      return OnError();
    session_created_ = true;
    ActivateSession();
  });
}

void Client::ActivateSession() {
  assert(session_created_);
  assert(!session_activated_);

  session_.Activate([this](opcua::StatusCode status_code) {
    if (!status_code)
      return OnError();
    session_activated_ = true;
    CreateSubscription();
  });
}

void Client::CreateSubscription() {
  opcua::client::SubscriptionParams params{
      500ms, // publishing_interval
      3000, // lifetime_count
      10000, // max_keepalive_count
      0, // max_notifications_per_publish
      true, // publishing_enabled
      0, // priority
  };

  subscription_.Create(params, [this](opcua::StatusCode status_code) {
    if (!status_code)
      return OnError();
  });
}

void Client::OnError() {
}*/

std::string ToString(opcua::StatusCode status_code) {
  if (status_code.IsGood())
    return "Good";
  else if (status_code.IsUncertain())
    return "Uncertain";
  else
    return "Bad";
}

int main() {
  try {
    std::cout << "Starting..." << std::endl;

    opcua::Platform platform;
    opcua::ProxyStub proxy_stub_{platform, MakeProxyStubConfiguration()};

    opcua::client::Channel channel{OpcUa_Channel_SerializerType_Binary};
    channel.set_url("opc.tcp://localhost:4840");
    channel.status_changed.Connect([&](opcua::StatusCode status_code) {
      assert(channel.status_code() == status_code);
      std::cout << "Channel status is " << ToString(status_code) << std::endl;
    });
    channel.Connect();

    opcua::client::Session session{channel};
    session.status_changed.Connect([&](opcua::StatusCode status_code) {
      assert(session.status_code() == status_code);
      std::cout << "Session status is " << ToString(status_code) << std::endl;
    });
    session.Create();

    opcua::client::Subscription subscription{session};
    subscription.status_changed.Connect([&](opcua::StatusCode status_code) {
      assert(session.status_code() == status_code);
      std::cout << "Subscription status is " << ToString(status_code) << std::endl;
    });
    subscription.Create(/*opcua::client::SubscriptionParams{
        500ms,  // publishing_interval
        3000,   // lifetime_count
        10000,  // max_keepalive_count
        0,      // max_notifications_per_publish
        true,   // publishing_enabled
        0,      // priority
    }*/);

    opcua::client::MonitoredItem item1{subscription};
    opcua::ReadValueId read_id;
    read_id.NodeId = opcua::NodeId{OpcUaId_Server_ServerStatus_State}.release();
    read_id.AttributeId = OpcUa_Attributes_Value;
    item1.Subscribe(std::move(read_id), [](opcua::MonitoredItemNotification& notification) {
      std::cout << "MonitoredItemNotification" << std::endl;
    });

    std::cout << "Running..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::cout << "Terminating..." << std::endl;

  } catch (const std::exception& e) {
    std::cout << "ERROR: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
