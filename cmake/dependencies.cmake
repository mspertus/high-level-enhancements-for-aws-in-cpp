# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(AWSSDK REQUIRED COMPONENTS s3 lambda)
find_package(aws-lambda-runtime)

# Determine at configure time whether std::expected is available.
# If yes, we don't need tl-expected at all (and avoid a misleading
# "package not found" warning). If no, tl-expected becomes a hard
# requirement so the user gets a clear error here rather than a
# cryptic missing-header error during compilation.
include(CheckCXXSourceCompiles)
set(_awslabs_prev_flags "${CMAKE_REQUIRED_FLAGS}")
if (NOT MSVC)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -std=c++23")
endif ()
check_cxx_source_compiles("
    #include <expected>
    int main() {
        std::expected<int, int> e = 42;
        return *e - 42;
    }
" AWSLABS_HAVE_STD_EXPECTED)
set(CMAKE_REQUIRED_FLAGS "${_awslabs_prev_flags}")

if (AWSLABS_HAVE_STD_EXPECTED)
    message(STATUS "Using std::expected (C++23)")
else ()
    message(STATUS "std::expected unavailable; falling back to tl::expected")
    find_package(tl-expected REQUIRED)
endif ()

if (BUILD_TESTING)
    message(STATUS "Building tests")
    # Testing dependency
    find_package(GTest 1.11 REQUIRED)
    include(GoogleTest) # for gtest_discover_tests()
endif ()
