#include "opcua/transport/binary/secure_channel.h"

#include "opcua/base/any_executor.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/transport/binary/client_secure_channel.h"
#include "opcua/transport/binary/client_transport.h"
#include "opcua/transport/binary/codec_utils.h"
#include "opcua/transport/binary/crypto.h"
#include "opcua/transport/binary/protocol.h"
#include "transport/transport.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// End-to-end coverage for the server-side Basic256Sha256 SignAndEncrypt
// SecureChannel. Rather than mock the crypto, these tests drive the real
// ClientSecureChannel against the real server SecureChannel through a loopback
// transport: the client encrypts/signs an OpenSecureChannel and service
// messages, the server decrypts/verifies them and replies in kind, proving the
// two sides agree on the OPC UA Part 6 §6.7 wire format and key derivation.
namespace opcua::binary {
namespace {

struct PemKeypair {
  std::string cert_pem;
  std::string private_key_pem;
};

// Generates a fresh 2048-bit RSA keypair wrapped in a self-signed X.509 cert,
// as PEM strings. Mirrors the helper in the client/crypto unit tests.
PemKeypair GenerateSelfSignedRsa() {
  EVP_PKEY* key = nullptr;
  EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
  EVP_PKEY_keygen(kctx, &key);
  EVP_PKEY_CTX_free(kctx);

  X509* cert = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_get_notBefore(cert), 0);
  X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60 * 24 * 30);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>("opc-ua-test"), -1, -1, 0);
  X509_set_issuer_name(cert, name);
  X509_sign(cert, key, EVP_sha256());

  PemKeypair result;
  {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    const auto len = BIO_get_mem_data(bio, &data);
    result.cert_pem.assign(data, len);
    BIO_free(bio);
  }
  {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const auto len = BIO_get_mem_data(bio, &data);
    result.private_key_pem.assign(data, len);
    BIO_free(bio);
  }
  X509_free(cert);
  EVP_PKEY_free(key);
  return result;
}

std::string AsString(const std::vector<char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

std::vector<char> AsVector(const std::string& bytes) {
  return {bytes.begin(), bytes.end()};
}

// Loopback transport that feeds every client frame into a real server
// SecureChannel and queues the server's reply back for the client to read.
// Hello is answered with an Acknowledge; SecureMessage payloads are echoed
// back through the server's symmetric response path.
struct LoopbackState {
  SecureChannel* server = nullptr;
  std::deque<std::string> incoming;  // server->client bytes
  std::vector<std::string> writes;   // client->server bytes
  bool opened = false;
  bool closed = false;
};

class LoopbackTransport {
 public:
  LoopbackTransport(transport::executor executor,
                    std::shared_ptr<LoopbackState> state)
      : executor_{std::move(executor)}, state_{std::move(state)} {}
  LoopbackTransport(LoopbackTransport&&) = default;
  LoopbackTransport& operator=(LoopbackTransport&&) = default;
  LoopbackTransport(const LoopbackTransport&) = delete;
  LoopbackTransport& operator=(const LoopbackTransport&) = delete;

  transport::awaitable<transport::error_code> open() {
    state_->opened = true;
    co_return transport::OK;
  }
  transport::awaitable<transport::error_code> close() {
    state_->closed = true;
    co_return transport::OK;
  }
  transport::awaitable<transport::expected<transport::any_transport>> accept() {
    co_return transport::ERR_NOT_IMPLEMENTED;
  }
  transport::awaitable<transport::expected<size_t>> read(std::span<char> data) {
    if (state_->incoming.empty()) {
      co_return size_t{0};
    }
    auto chunk = std::move(state_->incoming.front());
    state_->incoming.pop_front();
    if (chunk.size() > data.size()) {
      co_return transport::ERR_INVALID_ARGUMENT;
    }
    std::ranges::copy(chunk, data.begin());
    co_return chunk.size();
  }
  transport::awaitable<transport::expected<size_t>> write(
      std::span<const char> data) {
    state_->writes.emplace_back(data.begin(), data.end());
    const std::vector<char> frame{data.begin(), data.end()};
    if (frame.size() >= 3 && frame[0] == 'H' && frame[1] == 'E' &&
        frame[2] == 'L') {
      state_->incoming.push_back(AsString(EncodeAcknowledgeMessage(
          {.receive_buffer_size = 65535, .send_buffer_size = 65535})));
      co_return data.size();
    }

    auto result = co_await state_->server->HandleFrame(frame);
    if (result.outbound_frame.has_value() && !result.outbound_frame->empty()) {
      state_->incoming.push_back(AsString(*result.outbound_frame));
    }
    if (result.service_payload.has_value() && result.request_id.has_value()) {
      // Echo the request body back through the server's response path.
      auto response = state_->server->BuildServiceResponse(
          *result.request_id, std::move(*result.service_payload));
      if (!response.empty()) {
        state_->incoming.push_back(AsString(response));
      }
    }
    co_return data.size();
  }
  std::string name() const { return "LoopbackTransport"; }
  bool message_oriented() const { return false; }
  bool connected() const { return state_->opened && !state_->closed; }
  bool active() const { return true; }
  transport::executor get_executor() { return executor_; }

 private:
  transport::executor executor_;
  std::shared_ptr<LoopbackState> state_;
};

ClientSecureChannel::Security BuildClientSecurity(
    const PemKeypair& client_pk,
    const std::string& server_cert_pem) {
  ClientSecureChannel::Security s;
  s.security_policy_uri = std::string{kSecurityPolicyBasic256Sha256};
  s.security_mode = MessageSecurityMode::SignAndEncrypt;
  s.client_certificate =
      std::move(*crypto::LoadPemCertificate(client_pk.cert_pem));
  s.client_private_key =
      std::move(*crypto::LoadPemPrivateKey(client_pk.private_key_pem));
  s.server_certificate =
      std::move(*crypto::LoadPemCertificate(server_cert_pem));
  return s;
}

std::shared_ptr<const SecureChannelServerConfig> BuildServerConfig(
    const PemKeypair& server_pk,
    std::function<opcua::Status(std::span<const std::uint8_t>)> validator =
        {}) {
  auto certificate = crypto::LoadPemCertificate(server_pk.cert_pem);
  auto private_key = crypto::LoadPemPrivateKey(server_pk.private_key_pem);
  EXPECT_TRUE(certificate.ok());
  EXPECT_TRUE(private_key.ok());
  auto config = MakeSecureChannelServerConfig(
      std::move(*certificate), std::move(*private_key), /*allow_none=*/true,
      std::move(validator));
  EXPECT_TRUE(config.ok());
  return std::move(*config);
}

class SecureChannelServerBasic256Sha256Test : public ::testing::Test {
 protected:
  std::unique_ptr<ClientTransport> MakeClientTransport(
      const std::shared_ptr<LoopbackState>& state) {
    return std::make_unique<ClientTransport>(ClientTransportContext{
        .transport =
            transport::any_transport{LoopbackTransport{any_executor_, state}},
        .endpoint_url = "opc.tcp://localhost:4840",
        .limits = {},
    });
  }

  opcua::TestExecutor executor_;
  const transport::executor any_executor_ = executor_;
};

TEST_F(SecureChannelServerBasic256Sha256Test, OpenAndServiceRoundTrip) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  auto config = BuildServerConfig(server_pk);
  SecureChannel server{config, /*channel_id=*/77};

  auto state = std::make_shared<LoopbackState>();
  state->server = &server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());

  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  const auto open_status = opcua::WaitAwaitable(executor_, client.Open());
  ASSERT_TRUE(open_status.good());
  EXPECT_TRUE(client.opened());
  EXPECT_TRUE(server.opened());
  EXPECT_TRUE(server.secure());
  EXPECT_EQ(client.channel_id(), 77u);
  EXPECT_EQ(client.token_id(), server.token_id());

  // The server captured the client application instance certificate (DER).
  EXPECT_FALSE(server.client_certificate().empty());

  // A symmetric service request is encrypted+signed by the client, decrypted
  // and verified by the server, echoed, and decrypted back by the client.
  const std::uint32_t request_id = client.NextRequestId();
  const std::vector<char> payload{'p', 'i', 'n', 'g'};
  ASSERT_TRUE(opcua::WaitAwaitable(
                  executor_, client.SendServiceRequest(request_id, payload))
                  .good());
  auto response = opcua::WaitAwaitable(executor_, client.ReadServiceResponse());
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response->request_id, request_id);
  EXPECT_EQ(response->body, payload);
}

TEST_F(SecureChannelServerBasic256Sha256Test,
       RejectsUntrustedClientCertificate) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  // First capture a well-formed encrypted OPN frame from a real client.
  auto capture_config = BuildServerConfig(server_pk);
  SecureChannel capture_server{capture_config, /*channel_id=*/5};
  auto state = std::make_shared<LoopbackState>();
  state->server = &capture_server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  (void)opcua::WaitAwaitable(executor_, client.Open());
  ASSERT_GE(state->writes.size(), 2u);  // [0]=Hello, [1]=encrypted OPN
  const auto opn_frame = AsVector(state->writes[1]);

  // A server that rejects every client certificate must drop the channel.
  bool validator_called = false;
  auto reject_config = BuildServerConfig(
      server_pk, [&validator_called](std::span<const std::uint8_t> der) {
        validator_called = !der.empty();
        return opcua::Status{opcua::StatusCode::Bad};
      });
  SecureChannel rejecting_server{reject_config, /*channel_id=*/5};
  const auto result =
      opcua::WaitAwaitable(executor_, rejecting_server.HandleFrame(opn_frame));
  EXPECT_TRUE(validator_called);
  EXPECT_TRUE(result.close_transport);
  EXPECT_FALSE(result.outbound_frame.has_value());
  EXPECT_FALSE(rejecting_server.opened());
}

TEST_F(SecureChannelServerBasic256Sha256Test,
       RejectsSecureOpenWhenNotConfigured) {
  const auto client_pk = GenerateSelfSignedRsa();
  const auto server_pk = GenerateSelfSignedRsa();

  // Capture a Basic256Sha256 OPN frame.
  auto capture_config = BuildServerConfig(server_pk);
  SecureChannel capture_server{capture_config, /*channel_id=*/9};
  auto state = std::make_shared<LoopbackState>();
  state->server = &capture_server;
  auto client_transport = MakeClientTransport(state);
  ASSERT_TRUE(
      opcua::WaitAwaitable(executor_, client_transport->Connect()).good());
  ClientSecureChannel client{
      *client_transport, BuildClientSecurity(client_pk, server_pk.cert_pem)};
  (void)opcua::WaitAwaitable(executor_, client.Open());
  ASSERT_GE(state->writes.size(), 2u);
  const auto opn_frame = AsVector(state->writes[1]);

  // A None-only channel (no config) must refuse the secure OPN.
  SecureChannel none_only{/*channel_id=*/9};
  const auto result =
      opcua::WaitAwaitable(executor_, none_only.HandleFrame(opn_frame));
  EXPECT_TRUE(result.close_transport);
  EXPECT_FALSE(result.outbound_frame.has_value());
}

}  // namespace
}  // namespace opcua::binary
