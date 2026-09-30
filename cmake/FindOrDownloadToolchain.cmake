# An explicitly selected compiler always takes precedence over downloads.
if(PSN00BSDK_TC)
    find_program(_psn_gcc ${PSN00BSDK_TARGET}-gcc
        HINTS "${PSN00BSDK_TC}/bin" "${PSN00BSDK_TC}"
        NO_DEFAULT_PATH NO_CACHE REQUIRED)
else()
    find_program(_psn_gcc ${PSN00BSDK_TARGET}-gcc NO_CACHE)
endif()

option(PSN00BSDK_DOWNLOAD_TOOLCHAIN "Download a prebuilt toolchain if none was selected or found" ON)
if(NOT _psn_gcc)
    if(NOT PSN00BSDK_DOWNLOAD_TOOLCHAIN)
        message(FATAL_ERROR "Set PSN00BSDK_TC to a toolchain prefix or put ${PSN00BSDK_TARGET}-gcc in PATH.")
    endif()
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR
            "No native macOS toolchain found. Run bash tools/build-toolchain-macos.sh, then configure with "
            "-DPSN00BSDK_TC=${PROJECT_SOURCE_DIR}/build/toolchain-macos/install")
    elseif(NOT PSN00BSDK_TARGET STREQUAL "mipsel-none-elf")
        message(FATAL_ERROR "No prebuilt toolchain for ${PSN00BSDK_TARGET}; set PSN00BSDK_TC.")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(_psn_archive_os windows)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(_psn_archive_os linux)
    else()
        message(FATAL_ERROR "No prebuilt toolchain for this host; set PSN00BSDK_TC to a native installation.")
    endif()

    # Keep the existing v0.24 bootstrap as a fallback only. CI supplies GCC 16.1.
    set(_psn_prefix "${PROJECT_BINARY_DIR}/toolchain/${_psn_archive_os}")
    find_program(_psn_gcc ${PSN00BSDK_TARGET}-gcc
        PATHS "${_psn_prefix}/bin" NO_DEFAULT_PATH NO_CACHE)
    if(NOT _psn_gcc)
        set(_psn_archive "${PROJECT_BINARY_DIR}/gcc-12.3.0-${_psn_archive_os}.zip")
        file(DOWNLOAD
            "https://github.com/Lameguy64/PSn00bSDK/releases/download/v0.24/gcc-mipsel-none-elf-12.3.0-${_psn_archive_os}.zip"
            "${_psn_archive}" TLS_VERIFY ON SHOW_PROGRESS TIMEOUT 300 STATUS _psn_status)
        list(GET _psn_status 0 _psn_result)
        if(NOT _psn_result EQUAL 0)
            message(FATAL_ERROR "Toolchain download failed: ${_psn_status}")
        endif()
        file(MAKE_DIRECTORY "${_psn_prefix}")
        file(ARCHIVE_EXTRACT INPUT "${_psn_archive}" DESTINATION "${_psn_prefix}")
        find_program(_psn_gcc ${PSN00BSDK_TARGET}-gcc
            PATHS "${_psn_prefix}/bin" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    endif()
endif()
cmake_path(GET _psn_gcc PARENT_PATH _psn_bin)
cmake_path(GET _psn_bin PARENT_PATH _psn_prefix)
set(PSN00BSDK_TC "${_psn_prefix}" CACHE PATH "GCC toolchain installation directory" FORCE)
message(STATUS "Using toolchain: ${_psn_gcc}")
