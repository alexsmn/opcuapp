cmake_minimum_required(VERSION 3.25)
project(opcuapp CXX)

# opcuapp — self-contained OPC UA Binary / WebSocket stack.
#
# Vendors its own copies of the generic infrastructure and OPC UA domain
# types it needs (under opcua/base, opcua/scada, opcua/metrics, opcua/common)
# so it has no dependency on the SCADA `core` repo. The only external runtime
# dependencies are `transport` (the `net` library), OpenSSL and Boost.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OPCUAPP_BUILD_TESTS "Build opcuapp unit tests" OFF)

find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS json log locale)

# `transport` comes from the `net` library. A consuming build (e.g. the SCADA
# monorepo) usually already created the target; standalone builds must put the
# net directory on CMAKE_MODULE_PATH (or define OPCUAPP_TRANSPORT_DIR).
if(NOT TARGET transport)
  if(DEFINED OPCUAPP_TRANSPORT_DIR)
    list(APPEND CMAKE_MODULE_PATH "${OPCUAPP_TRANSPORT_DIR}")
  endif()
  find_package(transport QUIET)
endif()
if(NOT TARGET transport)
  message(FATAL_ERROR
    "opcuapp requires the `transport` target. Put the `net` directory on "
    "CMAKE_MODULE_PATH or pass -DOPCUAPP_TRANSPORT_DIR=<path to net>.")
endif()

file(GLOB_RECURSE OPCUAPP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/opcua/*.cpp")
list(FILTER OPCUAPP_SOURCES EXCLUDE REGEX "_unittest\\.cpp$")
# Select the right platform implementation of base::Time.
if(WIN32)
  list(FILTER OPCUAPP_SOURCES EXCLUDE REGEX "time_posix\\.cpp$")
else()
  list(FILTER OPCUAPP_SOURCES EXCLUDE REGEX "time_win\\.cpp$")
endif()

add_library(opcuapp STATIC ${OPCUAPP_SOURCES})
add_library(opcuapp::opcuapp ALIAS opcuapp)

# Include root is the repo root so the vendored + native sources resolve via
# the "opcua/..." prefix they already use.
target_include_directories(opcuapp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(opcuapp PUBLIC cxx_std_20)

target_link_libraries(opcuapp PUBLIC
  transport
  Boost::headers
  Boost::json
  Boost::log
  Boost::locale
)

# OpenSSL is consumed only by the Basic256Sha256 crypto helper under
# opcua/binary and the websocket TLS context; keep the link private.
target_link_libraries(opcuapp PRIVATE
  OpenSSL::SSL
  OpenSSL::Crypto
)

if(MSVC)
  target_compile_options(opcuapp PRIVATE
    /wd4702 # unreachable code in generated codec visitors
  )
endif()

if(OPCUAPP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
