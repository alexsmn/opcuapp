# opcuapp unit tests — builds every *_unittest.cpp under opcua/ into a single
# GoogleTest executable linked against the opcuapp library.
find_package(GTest REQUIRED)

file(GLOB_RECURSE OPCUAPP_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../opcua/*_unittest.cpp")

# NOTHING IS EXCLUDED HERE ANY MORE, AND NOTHING SHOULD BE. Every
# *_unittest.cpp under opcua/ compiles and runs. If a test stops building
# against a changed boundary, migrate it in the same change — do not add a
# filter here.
#
# Excluding a file is not free. This list once held twelve, and every one was a
# blind spot rather than dead weight:
#   - `session/server_subscription_unittest.cpp` sat here long enough that
#     ServerSubscription had NO compiled coverage, which is how a publish loop
#     that emitted one notification per cycle — the 2026-08-02 S3 starvation
#     defect — reached a live cluster.
#   - `transport/binary/runtime_unittest.cpp` hid two more: RuntimeContext
#     silently dropped `operation_limits` when building its inner
#     ServerRuntimeContext (so the Binary transport could not enforce limits the
#     address space advertised), and BuildRuntimeErrorResponse matched no branch
#     for the generated ua:: response types, answering a rejected request with a
#     default (Good, empty) response instead of Bad_SessionIdInvalid.
#
# The shared fixtures the migrated suites use are
# `opcua/monitored/test/fake_monitored_item_subscription.h` (the replacement for
# the deleted `opcua/monitored/item_factory_subscription.h`) and the recording
# `ScriptedServices` in `opcua/session/server_runtime_contract_test.h`, which
# replaced the deleted per-service gMock layer.

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
