# opcuapp unit tests — builds every *_unittest.cpp under opcua/ into a single
# GoogleTest executable linked against the opcuapp library.
find_package(GTest REQUIRED)

file(GLOB_RECURSE OPCUAPP_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../opcua/*_unittest.cpp")

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
