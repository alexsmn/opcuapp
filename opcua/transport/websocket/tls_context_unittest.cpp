#include "opcua/websocket/tls_context.h"

#include <boost/asio/ssl/context.hpp>
#include <gtest/gtest.h>

namespace opcua::ws {
namespace {

constexpr auto kTestCertificatePem = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUQWAR+40WE34MoTuKigrGeiGT5ycwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDQyMDE4NTczMloXDTM2MDQx
NzE4NTczMlowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAsUhr1SNMP7HMVoUgPK1j2ecIvRSz0PiGagh5kOL6HETy
Eo3MTux9LheWcvhhMvpm3HNOIx6Q8iModo77le+nyYxExrQ4xNFTvwsFhkpso2sQ
C9fWO7uabAFhMSJZZq5vs8y4o3aMQNTeSBWzkoX0ebdzkBn0YbLa6Gvas87oF50g
NWiOKJNNmZL6rC2iSMI+2c/fq7UrPkmDd6VObFXO7ItTFXbqeXPiynuMnLXf5sGU
V3VKP8PyVGR7QJq7LRa30BCsfSTAK6BlVIcPkDMA+1uVM6lkPI103axRF3UhwwU6
B8WOpyafhul18ZiFtSYz78x5K3kYrSvYjOOb1YeHEwIDAQABo1MwUTAdBgNVHQ4E
FgQUaGBbcBpQLHhmkVgc1Eg2OOXTLM0wHwYDVR0jBBgwFoAUaGBbcBpQLHhmkVgc
1Eg2OOXTLM0wDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAeEkK
63ra3vPoBp49UWiRgPfroFKbX/AI01/r0WRsSDzwOPoKRmSpuwdcXfT3B3ilzmDX
sfiAJJQtvcgm3V3vNzWmks+4dLS0G+bRwBxih5b16xAWbUcUgty9TX3HOlAzqdOh
BnoQQczubSQv2/0AzfxZ06EGpwrlFrTzXJRpmQRoqo8xGEwgiQ10A3aXYYyxydzj
ChS5hFDZ4P6MA01VI2y5dgIUiA+3uEWOt2SJgexDaFk7ZmpVqzh4KHV0deSyG3Gt
QoPGNJO4DZVyCXKd4iOg40H4JRc2fIlMykOklAD7ukuUpGNeEPKzSQjakxKYKiX8
NGfJbg7n4a46yf+S6A==
-----END CERTIFICATE-----
)";

constexpr auto kTestPrivateKeyPem = R"(-----BEGIN PRIVATE KEY-----
MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQCxSGvVI0w/scxW
hSA8rWPZ5wi9FLPQ+IZqCHmQ4vocRPISjcxO7H0uF5Zy+GEy+mbcc04jHpDyIyh2
jvuV76fJjETGtDjE0VO/CwWGSmyjaxAL19Y7u5psAWExIllmrm+zzLijdoxA1N5I
FbOShfR5t3OQGfRhstroa9qzzugXnSA1aI4ok02ZkvqsLaJIwj7Zz9+rtSs+SYN3
pU5sVc7si1MVdup5c+LKe4yctd/mwZRXdUo/w/JUZHtAmrstFrfQEKx9JMAroGVU
hw+QMwD7W5UzqWQ8jXTdrFEXdSHDBToHxY6nJp+G6XXxmIW1JjPvzHkreRitK9iM
45vVh4cTAgMBAAECggEAD+7UUimD9s2B8dyxEwL6UGElNekgaA2N9wWf91eO5u+D
WguIaydx8KyKBvcvtScwC2wJf7qFiF2Ei3M6RTVuvPxwSfN0jqvJfQf+jR0vOliq
7oWNaXzo2gAdvg66PjI7M8uYZIiI/mKjP5NDuk1ztWS5bCAJCKbMacsXssVLsqN0
ENH+/vLsawjvD+/UQQGr4FUvw0QCYz1FA5C4XCyHEidMORRSOhh7yWB38lbO0StU
StnWGdeYxNz1+VGJwfckuvvKVl8A6vY8So5LPsHS2aqDXcAwmSkJgd78k/eaW1V7
bQ7kn8NtXECrphIYcHu4/yuvwp+Y9Avc3pCtC4etQQKBgQDEjupDiFsOM3RDNfoj
9fQc/jC9I61TBoi9kWIj0UMR1nCi+SNlxm5uV4gRQcTs4MSxFGqgcdCm8Bb5MNld
dxsMJYOOqDSciMpyEdwCEtk17ctR48+Vsan22xxzqezsI7Skvzu0FzBs7g5cnE79
hxeV3+qgrirnO7xwe6dJLrOCIwKBgQDm5UB+kS9SWc31Dhxc0fuOhDa7PhDQKo4V
ZsCEtc7hpytcjrLSC9Ru5Er1tp3L/oF5jQLMalhdMR1QagG7cJOAdT4K3+tjnags
E4c/Vw5xzRdjuWZEAleZATyRwonvFXSf868Sg8op6xZjMFq4qPb+8mJexzinVOM9
ipsA2CveUQKBgQDCosJXHS8NYOY/p7OK6IJSM2MP58Q58r50+QG1dgJ0J2Rh/VKP
9W5k1UhnzjiyV+BteUoclpeGtzgIida0Nr0RyhP7r5RpbQsK6aRyaTetr0smS+/C
y6sCRvZlkl6JdtHqUXNNYakSNKkEC8QsSRmRz6kGc3EIiJ6Qw+FjFlurAQKBgQDZ
PWch7j3E0IPMBfu/hT2WiGTqZOnywacvEZ8e/ePpQZy1l/k9US4NK7QvXSM4VHvD
Pl4csA31mIlJKIP6tF/DZAv8tVNGRYZ9+d2tRZ5sihdwl3ZVlJKQfa5cQdn/XYN+
HwtgcyjZqbtFlbA1v5usoabWH8D5BxBKzccq0zjrEQKBgQCaSvtY2PveeWHZBOlN
aFTbyLdyBHdhwpYZm3vKQ3MoOtnQGRq1BlQk7ojruxGhKMTnJhNP2cSnDdaUJB/S
urjYLs2lpMW81+/p1qt5QCP+hVU+/Y7rdCXmTQjsy+9rAf3VOupMisj2gmgX8ASm
mCzxKIlbzMnhGhGlzdKwqs5Uhw==
-----END PRIVATE KEY-----
)";

TEST(TlsContextTest, LoadsValidPemCertificateAndKey) {
  boost::asio::ssl::context context{boost::asio::ssl::context::tls_server};

  EXPECT_EQ(
      ConfigureServerTlsContext(
          context,
          {.certificate_chain_pem = kTestCertificatePem,
           .private_key_pem = kTestPrivateKeyPem}),
      transport::OK);
}

TEST(TlsContextTest, RejectsInvalidCertificatePem) {
  boost::asio::ssl::context context{boost::asio::ssl::context::tls_server};

  EXPECT_NE(
      ConfigureServerTlsContext(
          context,
          {.certificate_chain_pem = "not a certificate",
           .private_key_pem = kTestPrivateKeyPem}),
      transport::OK);
}

TEST(TlsContextTest, RejectsInvalidPrivateKeyPem) {
  boost::asio::ssl::context context{boost::asio::ssl::context::tls_server};

  EXPECT_NE(
      ConfigureServerTlsContext(
          context,
          {.certificate_chain_pem = kTestCertificatePem,
           .private_key_pem = "not a private key"}),
      transport::OK);
}

}  // namespace
}  // namespace opcua::ws
