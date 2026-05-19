/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AWSLABS_ENHANCED_CRT_HTTP_CLIENT_H
#define AWSLABS_ENHANCED_CRT_HTTP_CLIENT_H

#include <cstdint>

namespace AwsLabs::Enhanced::Crt {

// Install a CRT-based Aws::Http::HttpClientFactory as the global SDK factory.
//
// Must be called BEFORE any AWS client (LambdaClient, S3Client, etc.) is
// constructed; the SDK caches the factory at client-construction time.
//
// EnhancedLambdaClient calls this automatically via std::call_once on first
// construction, so most users never need to call it directly. Call it
// explicitly only if you want CRT transport for other AWS clients before
// any EnhancedLambdaClient exists, or if you want to control the I/O thread
// count.
//
// `eventLoopThreads`: size of the CRT I/O thread pool. 0 means
// auto-detect (= hardware concurrency).
//
// Safe to call multiple times; subsequent calls after the first are no-ops.
void InstallCrtHttpFactory(uint16_t eventLoopThreads = 0);

}  // namespace AwsLabs::Enhanced::Crt

#endif  // AWSLABS_ENHANCED_CRT_HTTP_CLIENT_H
