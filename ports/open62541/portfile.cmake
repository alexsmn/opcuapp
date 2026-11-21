vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO open62541/open62541
    REF "v${VERSION}"
    SHA512 2ef439feaa2bb413902cce4e92fdcf874edacfec1972408c4a3fb30c41aee398957a603e8be763b3627c3cee65e82526fa2f12b384b5c1b0a79f5da9031b8320
    HEAD_REF master
    PATCHES
      android.patch
)

# disable docs
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt" "add_subdirectory(doc)" "")
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt" "include(linting_target)" "")

# do not enable LTO by default
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt" "set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)" "")

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        diagnostics UA_ENABLE_DIAGNOSTICS
        discovery UA_ENABLE_DISCOVERY
        historizing UA_ENABLE_HISTORIZING
        methodcalls UA_ENABLE_METHODCALLS
        subscriptions UA_ENABLE_SUBSCRIPTIONS
        subscriptions-events UA_ENABLE_SUBSCRIPTIONS_EVENTS
        # OVERLAY (telecontrol, ports/README.md): Alarms & Conditions, which
        # upstream's port does not expose. open62541 refuses it without events
        # and the full namespace zero, so the feature brings both.
        alarms-conditions UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS
)

if("alarms-conditions" IN_LIST FEATURES)
    # The full namespace zero is generated from the OPC Foundation's nodeset,
    # which open62541 carries as the deps/ua-nodeset git submodule -- absent
    # from the release tarball, so its configure would try `git submodule
    # update` and fail. Fetch the commit v1.4.22 pins (as did v1.4.21) instead.
    vcpkg_from_github(
        OUT_SOURCE_PATH UA_NODESET_PATH
        REPO OPCFoundation/UA-Nodeset
        REF d1bb6a22125bd7cd986272b1ee98a18a91d76fff
        SHA512 4dc3dbe434d686874600776ac690cd026e903ee6e79d54b9f662642457327cc24eaf15668485aacf606896278e93d01df36b833385847d2ef1ab895a19f3b32c
        HEAD_REF latest
    )
    list(APPEND FEATURE_OPTIONS
        "-DUA_NAMESPACE_ZERO=FULL"
        "-DUA_NODESET_DIR=${UA_NODESET_PATH}")
endif()

if("openssl" IN_LIST FEATURES)
    set(OPEN62541_ENCRYPTION_OPTIONS -DUA_ENABLE_ENCRYPTION=OPENSSL)
    if("mbedtls" IN_LIST FEATURES)
        message(WARNING "Only one encryption method can be used. When both [openssl] and [mbedtls] "
            "are on, openssl is used. To use [mbedtls], don't enable [openssl]. To suppress this "
            "message, don't enable [mbedtls]")
    endif()
elseif("mbedtls" IN_LIST FEATURES)
    set(OPEN62541_ENCRYPTION_OPTIONS -DUA_ENABLE_ENCRYPTION=MBEDTLS)
endif()

if("multithreading" IN_LIST FEATURES)
    set(OPEN62541_MULTITHREADING_OPTIONS -DUA_MULTITHREADING=100)
endif()

vcpkg_find_acquire_program(PYTHON3)
get_filename_component(PYTHON3_DIR "${PYTHON3}" DIRECTORY)
vcpkg_add_to_path("${PYTHON3_DIR}")

if(VCPKG_TARGET_IS_ANDROID)
  list(APPEND FEATURE_OPTIONS "-DUA_ARCHITECTURE=posix")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        ${OPEN62541_ENCRYPTION_OPTIONS}
        ${OPEN62541_MULTITHREADING_OPTIONS}
        "-DOPEN62541_VERSION=v${VERSION}"
        -DUA_ENABLE_DEBUG_SANITIZER=OFF
        -DUA_MSVC_FORCE_STATIC_CRT=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/open62541")
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/open62541/tools"
)

vcpkg_install_copyright(
    COMMENT [[
PCG and base64 reference Apache-2.0 and BSD terms without including the full texts.
libc_time only names musl's MIT license; parse_num includes MIT terms but no copyright holder.
The OPC Foundation schema is installed and also generates the library's standard data types.
]]
    FILE_LIST
        "${SOURCE_PATH}/LICENSE"
        "${SOURCE_PATH}/LICENSE-CC0"
        "${SOURCE_PATH}/deps/README.md"
        "${SOURCE_PATH}/tools/schema/Opc.Ua.Types.bsd"
)
