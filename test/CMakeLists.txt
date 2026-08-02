# opcuapp unit tests — builds every *_unittest.cpp under opcua/ into a single
# GoogleTest executable linked against the opcuapp library.
find_package(GTest REQUIRED)

file(GLOB_RECURSE OPCUAPP_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../opcua/*_unittest.cpp")

# These tests still exercise the removed legacy single-item MonitoredItem
# fixture layer or pre-callback service mocks. Keep the standalone test target
# focused on tests that compile against the current OPC UA protocol boundary
# until those fixtures are rewritten to the subscription-batch API.
#
# Excluding a file is not free: `session/server_subscription_unittest.cpp` sat
# here long enough that ServerSubscription had NO compiled coverage at all,
# which is how a publish loop that emitted one notification per cycle — the
# 2026-08-02 S3 starvation defect — survived to a live cluster. It was rewritten
# against the current callback boundary and is back in the glob; the fixture it
# uses, `opcua/monitored/test/fake_monitored_item_subscription.h`, is the
# replacement for the missing `opcua/monitored/item_factory_subscription.h`
# these remaining files still include, and is the migration path for them.
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/client/client_session_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/server/service_handler_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/session/server_runtime_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/session/server_session_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/transport/binary/client_server_e2e_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/transport/binary/runtime_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/transport/binary/service_dispatcher_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/transport/websocket/server_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/transport/websocket/service_handler_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/transport/websocket/session_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/transport/websocket/subscription_unittest\\.cpp$")
list(FILTER OPCUAPP_TEST_SOURCES EXCLUDE REGEX
  "/opcua/transport/websocket/websocket_server_unittest\\.cpp$")

add_executable(opcuapp_unittests ${OPCUAPP_TEST_SOURCES})

target_link_libraries(opcuapp_unittests PRIVATE
  opcuapp
  GTest::gtest
  GTest::gtest_main
  GTest::gmock
  # crypto_unittest / client_secure tests build RSA fixtures via OpenSSL directly.
  OpenSSL::SSL
  OpenSSL::Crypto
)

include(GoogleTest)
gtest_discover_tests(opcuapp_unittests DISCOVERY_MODE PRE_TEST)
