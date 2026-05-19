# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Locates the AWS Common Runtime (CRT) C libraries used by the CRT-based
# HTTP transport. These libraries don't ship CMake config files, so we find
# them with find_library / find_path. On failure we abort with a clear
# message rather than silently degrading.

# Headers we'll #include from src/crt/crt_http_client.cpp
find_path(AWSLABS_CRT_HTTP_INCLUDE_DIR
    NAMES aws/http/connection_manager.h
    HINTS /usr/local/include /usr/include
)
if (NOT AWSLABS_CRT_HTTP_INCLUDE_DIR)
    message(FATAL_ERROR
        "AWSLABS_ENABLE_CRT=ON but the CRT HTTP headers (aws/http/connection_manager.h) "
        "were not found. Install aws-c-http and friends, or configure with "
        "-DAWSLABS_ENABLE_CRT=OFF to use the libcurl-based transport.")
endif ()

# Each CRT C library lives in its own .so. List them in dependency order
# (low-level first, high-level last) so static-link orderings stay sane.
set(_awslabs_crt_libs
    aws-c-common
    aws-c-cal
    aws-c-io
    aws-c-compression
    aws-c-http
    s2n
)

set(AWSLABS_CRT_LIBRARIES "")
foreach (_lib IN LISTS _awslabs_crt_libs)
    find_library(_awslabs_crt_${_lib}
        NAMES ${_lib}
        HINTS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu
    )
    if (NOT _awslabs_crt_${_lib})
        message(FATAL_ERROR
            "AWSLABS_ENABLE_CRT=ON but lib${_lib}.so was not found. "
            "Install the AWS CRT C libraries or configure with -DAWSLABS_ENABLE_CRT=OFF.")
    endif ()
    list(APPEND AWSLABS_CRT_LIBRARIES "${_awslabs_crt_${_lib}}")
endforeach ()

message(STATUS "Found AWS CRT C libraries: ${AWSLABS_CRT_LIBRARIES}")
