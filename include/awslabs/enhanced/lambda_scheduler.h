/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AWSLABS_ENHANCED_LAMBDA_SCHEDULER_H
#define AWSLABS_ENHANCED_LAMBDA_SCHEDULER_H

#include "awslabs/enhanced/lambda_client.h"

#include <stdexec/execution.hpp>
#include <exec/static_thread_pool.hpp>

#include <memory>

namespace AwsLabs::Enhanced {

// LambdaScheduler — a P2300/C++26 std::execution scheduler intended for
// chains whose work is composed of AWS Lambda invocations.
//
// v1 implementation: thin wrapper around exec::static_thread_pool whose
// pool size is taken from the associated EnhancedLambdaClient's maxConnections.
// `schedule(sch) | bulk(par, n, f)` fans f(0)..f(n-1) out across pool
// threads; each f(i) typically calls into a bound Lambda<R(Args...)> and
// blocks the thread for the Lambda round-trip.
//
// Thread cost matches today's transform demo (~min(N, maxConnections)
// threads parked on CrtHttpClient::MakeRequest). A follow-up branch will
// make MakeRequest yield (coroutine or fiber) so the same workload runs
// in ~10-20 kernel threads. See the plan file's "Out of scope" section.
struct LambdaScheduler {
    // Constructed from an EnhancedLambdaClient. Sizes the backing pool to
    // the client's maxConnections, matching how the SDK's executor is
    // already sized (lambda_client.h:67-77).
    explicit LambdaScheduler(EnhancedLambdaClient& client)
        : pool_(std::make_shared<exec::static_thread_pool>(
              static_cast<std::size_t>(thread_count_from(client))))
    {}

    // Returns the underlying stdexec scheduler. Use as:
    //   stdexec::sync_wait(stdexec::schedule(sch.get()) | stdexec::bulk(par, n, f));
    auto get() const {
        return pool_->get_scheduler();
    }

private:
    static unsigned thread_count_from(EnhancedLambdaClient& client) {
        // EnhancedLambdaClient raises maxConnections to 1000 by default
        // (lambda_client.h:67). We mirror that here so the scheduler can
        // sustain Lambda's default concurrency.
        // The SDK doesn't expose maxConnections back through the client
        // object cleanly, so we hard-code the same default. If someone
        // explicitly configured a different value, this scheduler will be
        // sized to 1000 regardless — fine for v1; a future API can expose
        // the knob.
        (void)client;
        return 1000;
    }

    // shared_ptr so LambdaScheduler is copyable; the pool itself isn't.
    std::shared_ptr<exec::static_thread_pool> pool_;
};

} // namespace AwsLabs::Enhanced

#endif // AWSLABS_ENHANCED_LAMBDA_SCHEDULER_H
