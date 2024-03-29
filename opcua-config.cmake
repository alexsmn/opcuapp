project(OPCUA)

if(WIN32)
  set(OPCUA_PLATFORM "win32")
else()
  set(OPCUA_PLATFORM "linux")
endif()

list(APPEND OPCUA_INCLUDE_DIRS
  "${OPCUA_ROOT_DIR}/Stack/core"
  "${OPCUA_ROOT_DIR}/Stack/platforms/${OPCUA_PLATFORM}"
  "${OPCUA_ROOT_DIR}/Stack/proxystub/clientproxy"
  "${OPCUA_ROOT_DIR}/Stack/proxystub/serverstub"
  "${OPCUA_ROOT_DIR}/Stack/securechannel"
  "${OPCUA_ROOT_DIR}/Stack/stackcore"
  "${OPCUA_ROOT_DIR}/Stack/transport/https"
  "${OPCUA_ROOT_DIR}/Stack/transport/tcp"
)

file(GLOB OPCUA_SOURCES
  "${OPCUA_ROOT_DIR}/Stack/core/*.*"
  "${OPCUA_ROOT_DIR}/Stack/platforms/${OPCUA_PLATFORM}/*.*"
  "${OPCUA_ROOT_DIR}/Stack/securechannel/*.*"
  "${OPCUA_ROOT_DIR}/Stack/proxystub/clientproxy/*.*"
  "${OPCUA_ROOT_DIR}/Stack/proxystub/serverstub/*.*"
  "${OPCUA_ROOT_DIR}/Stack/stackcore/*.*"
  "${OPCUA_ROOT_DIR}/Stack/transport/https/*.*"
  "${OPCUA_ROOT_DIR}/Stack/transport/tcp/*.*"
)

if(WIN32)
  add_library(OPCUA SHARED ${OPCUA_SOURCES})
else()
  add_library(OPCUA ${OPCUA_SOURCES})
endif()

target_include_directories(OPCUA PUBLIC
  ${OPCUA_INCLUDE_DIRS}
  ${OPENSSL_INCLUDE_DIR}
)

find_package(OpenSSL REQUIRED)
target_link_libraries(OPCUA PUBLIC OpenSSL::SSL OpenSSL::Crypto)

target_compile_definitions(OPCUA PRIVATE "$<$<CONFIG:DEBUG>:OPCUA_MUTEX_ERROR_CHECKING>")

if(WIN32)
  # TODO: Detect target Windows version.
  target_compile_definitions(OPCUA
    PRIVATE _GUID_CREATE_NOT_AVAILABLE
  )

  target_compile_definitions(OPCUA
    PRIVATE _UA_STACK_BUILD_DLL
    INTERFACE _UA_STACK_USE_DLL
  )

  target_compile_options(OPCUA PRIVATE
    /wd4047
    /wd4090
  )

  target_link_libraries(OPCUA PRIVATE
    Crypt32
    Ws2_32
  )
endif()
