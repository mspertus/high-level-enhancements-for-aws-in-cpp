/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

// Skeleton implementation. At this stage InstallCrtHttpFactory is a no-op
// that just announces it was called, so we can verify the wiring (header,
// library target, link from EnhancedLambdaClient) end-to-end before
// committing to the real CRT plumbing.

#include "awslabs/enhanced/crt/crt_http_client.h"

#include <iostream>
#include <mutex>

namespace AwsLabs::Enhanced::Crt {

void InstallCrtHttpFactory(uint16_t eventLoopThreads) {
    static std::once_flag installed;
    std::call_once(installed, [eventLoopThreads]() {
        // TODO: actually create a CrtHttpClientFactory and call
        //   Aws::Http::SetHttpClientFactory(factory)
        // For now just log so we can prove the wiring is right.
        std::cerr << "[awslabs/crt] InstallCrtHttpFactory(eventLoopThreads="
                  << eventLoopThreads << ") -- skeleton, no-op for now\n";
    });
}

}  // namespace AwsLabs::Enhanced::Crt
