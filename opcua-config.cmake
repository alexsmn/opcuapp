find_package(openssl REQUIRED)

list(APPEND OPCUA_INCLUDE_DIRS
  "${OPCUA_ROOT_DIR}/Stack/core"
  "${OPCUA_ROOT_DIR}/Stack/platforms/win32"
  "${OPCUA_ROOT_DIR}/Stack/proxystub/clientproxy"
  "${OPCUA_ROOT_DIR}/Stack/proxystub/serverstub"
  "${OPCUA_ROOT_DIR}/Stack/securechannel"
  "${OPCUA_ROOT_DIR}/Stack/stackcore"
  "${OPCUA_ROOT_DIR}/Stack/transport/https"
  "${OPCUA_ROOT_DIR}/Stack/transport/tcp"
)

file(GLOB OPCUA_SOURCES
  "${OPCUA_ROOT_DIR}/Stack/core/*.*"
  "${OPCUA_ROOT_DIR}/Stack/platforms/win32/*.*"
  "${OPCUA_ROOT_DIR}/Stack/securechannel/*.*"
  "${OPCUA_ROOT_DIR}/Stack/proxystub/clientproxy/*.*"
  "${OPCUA_ROOT_DIR}/Stack/proxystub/serverstub/*.*"
  "${OPCUA_ROOT_DIR}/Stack/stackcore/*.*"
  "${OPCUA_ROOT_DIR}/Stack/transport/https/*.*"
  "${OPCUA_ROOT_DIR}/Stack/transport/tcp/*.*"
)

add_library(OPCUA SHARED ${OPCUA_SOURCES})

target_compile_definitions(OPCUA
  PRIVATE -D_UA_STACK_BUILD_DLL
  INTERFACE -D_UA_STACK_USE_DLL
)

if(WIN32)
  # TODO: Detect target Windows version.
  target_compile_definitions(OPCUA
    PRIVATE -D_GUID_CREATE_NOT_AVAILABLE
  )
endif()

target_include_directories(OPCUA PUBLIC
  ${OPCUA_INCLUDE_DIRS}
  ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(OPCUA
  Crypt32
  Rpcrt4
  Ws2_32
  ${OPENSSL_LIBRARIES}
)
