cmake_minimum_required(VERSION 3.25)
project(opcuapp CXX)

# opcuapp — self-contained OPC UA Binary / WebSocket stack.
#
# Vendors its own copies of the generic infrastructure and OPC UA domain
# types it needs (under opcua/base, opcua/types, opcua/services, opcua/events, opcua/client, opcua/session, opcua/server, opcua/metrics)
# so it has no dependency on the SCADA `core` repo. The only external runtime
# dependencies are `transport` (the `net` library), OpenSSL and Boost.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OPCUAPP_BUILD_TESTS "Build opcuapp unit tests" OFF)

find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS json log locale)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The OPC UA type system is generated from the vendored OPC Foundation schema
# (schema/, pinned in schema/VERSION) rather than transcribed by hand; see
# schema/README.md. The built-in types it builds on (NodeId, Variant,
# DataValue, ...) stay hand-written under opcua/types/.
set(OPCUAPP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/opcua/ua")
set(OPCUAPP_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/schema")
set(OPCUAPP_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_ua_types.py")

set(OPCUAPP_GENERATED_HEADERS
    "${OPCUAPP_GENERATED_DIR}/ua_types.h"
    "${OPCUAPP_GENERATED_DIR}/ua_encoding_ids.h"
    "${OPCUAPP_GENERATED_DIR}/ua_status_codes.h"
    "${OPCUAPP_GENERATED_DIR}/ua_binary_codec.h"
    "${OPCUAPP_GENERATED_DIR}/ua_json_codec.h")

# Each codec (binary, JSON) is split across translation units so no single one
# has to hold all 357 structs' worth of encode + decode. Keep this count in
# step with the --codec-shards argument below.
set(OPCUAPP_CODEC_SHARDS 4)
set(OPCUAPP_GENERATED_SOURCES "")
math(EXPR _opcuapp_last_shard "${OPCUAPP_CODEC_SHARDS} - 1")
foreach(_shard RANGE ${_opcuapp_last_shard})
  list(APPEND OPCUAPP_GENERATED_SOURCES
       "${OPCUAPP_GENERATED_DIR}/ua_binary_codec_${_shard}.cpp"
       "${OPCUAPP_GENERATED_DIR}/ua_json_codec_${_shard}.cpp")
endforeach()

add_custom_command(
  OUTPUT ${OPCUAPP_GENERATED_HEADERS} ${OPCUAPP_GENERATED_SOURCES}
  COMMAND Python3::Interpreter "${OPCUAPP_GENERATOR}"
          --schema "${OPCUAPP_SCHEMA_DIR}" --out "${OPCUAPP_GENERATED_DIR}"
          --codec-shards ${OPCUAPP_CODEC_SHARDS}
  DEPENDS "${OPCUAPP_GENERATOR}"
          "${OPCUAPP_SCHEMA_DIR}/Opc.Ua.Types.bsd"
          "${OPCUAPP_SCHEMA_DIR}/NodeIds.csv"
          "${OPCUAPP_SCHEMA_DIR}/StatusCode.csv"
          "${OPCUAPP_SCHEMA_DIR}/opc.ua.services.jsonschema.json"
  COMMENT "Generating OPC UA types and codecs from the vendored schema"
  VERBATIM)

add_custom_target(opcuapp_generate
  DEPENDS ${OPCUAPP_GENERATED_HEADERS} ${OPCUAPP_GENERATED_SOURCES})
set_source_files_properties(
  ${OPCUAPP_GENERATED_HEADERS} ${OPCUAPP_GENERATED_SOURCES}
  PROPERTIES GENERATED TRUE)

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
# Select the right platform implementation of opcua::DateTime.
if(WIN32)
  list(FILTER OPCUAPP_SOURCES EXCLUDE REGEX "date_time_posix\\.cpp$")
else()
  list(FILTER OPCUAPP_SOURCES EXCLUDE REGEX "date_time_win\\.cpp$")
endif()

add_library(opcuapp STATIC ${OPCUAPP_SOURCES} ${OPCUAPP_GENERATED_SOURCES})
add_library(opcuapp::opcuapp ALIAS opcuapp)

# Generation must run before any opcuapp source compiles, since they include
# the generated opcua/ua/*.h.
add_dependencies(opcuapp opcuapp_generate)

# Include root is the repo root so the vendored + native sources resolve via
# the "opcua/..." prefix they already use; the generated root is second so the
# generated headers resolve under the same "opcua/ua/" prefix.
target_include_directories(opcuapp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
                                          "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(opcuapp PUBLIC cxx_std_20)

target_link_libraries(opcuapp PUBLIC
  transport
  Boost::headers
  Boost::json
  Boost::log
  Boost::locale
)

# OpenSSL is consumed only by the Basic256Sha256 crypto helper under
# opcua/transport/binary and the websocket TLS context; keep the link private.
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
