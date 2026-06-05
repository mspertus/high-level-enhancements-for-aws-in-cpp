/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sibling of central_limit_theorem.cpp that runs the same statistical
 * workload via C++26 senders/receivers (std::execution / stdexec)
 * instead of std::transform. Uses the LambdaScheduler from
 * <awslabs/enhanced/lambda_scheduler.h> and standard bulk.
 *
 * Reuses the deployed exp_mean(exp_parameters) Lambda unchanged; the proxy
 * closure handed to bulk translates bulk's int index into spec[i] so the
 * wire payload matches what the existing transform demo sends.
 */

#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <algorithm>
#include <execution>      // for std::execution::par
#include <functional>
#include <vector>
#include <cstdlib>

#include <fmt/format.h>
#include <cxxopts.hpp>

#include <stdexec/execution.hpp>
#include <exec/static_thread_pool.hpp>

#include "exp_mean.h"
#include "awslabs/enhanced/lambda_client.h"
#include "awslabs/enhanced/lambda_scheduler.h"
#include "awslabs/enhanced/Aws.h"

using std::map;
using std::vector;
using std::accumulate;
using fmt::format;
using std::string;
using std::ostream;
using std::cout;
using std::cerr;
using namespace AwsLabs::Enhanced;

AwsApi api;
EnhancedLambdaClient client;
auto cloud_exp_mean = BIND_AWS_LAMBDA(client, exp_mean, "exp_mean");
LambdaScheduler lambda_sch{client};

struct opts {
    double lambda;
    unsigned samples;
    unsigned experiments;
    bool cloud;
    string policy;
};

std::string description = R"(
Senders/receivers (C++26 std::execution) variant of the central limit
theorem demo. Runs `schedule(sch) | bulk(policy, n, proxy) | sync_wait`
where the scheduler and policy together determine where the work runs:

  -p seq               local, sequential
  -p par               local, parallel (CPU pool)
  -p par_unseq         local, parallel + may vectorize
  -p cloud             AWS Lambda fan-out via LambdaScheduler
  -c -p seq            cloud, sequential (one Lambda call at a time)

The local policies call exp_mean() locally; -p cloud (and -c) call the
deployed exp_mean Lambda via cloud_exp_mean.

For full instructions, including how to deploy the function to the cloud, see
https://github.com/awslabs/high-level-enhancements-for-aws-in-cpp/tree/main/doc#central-limit-theorem-example
)";

auto get_opts(int argc, char* argv[]) {
    cxxopts::Options options(
        "central_limit_theorem_exec",
        "senders/receivers cloud demo for the central limit theorem");
    options.add_options()
        ("l,lambda",      "Lambda parameter for exponential distribution",
            cxxopts::value<double>()->default_value("1"))
        ("s,samples",     "Number of samples in each experiment",
            cxxopts::value<unsigned>()->default_value("50000000"))
        ("e,experiments", "Number of experiments to perform",
            cxxopts::value<unsigned>()->default_value("500"))
        ("c,cloud",       "Run on Lambda (force when policy=seq)",
            cxxopts::value<bool>()->default_value("false"))
        ("p,policy",      "seq, par, par_unseq, cloud",
            cxxopts::value<string>())
        ("h,help",        "Print usage");
    auto result = options.parse(argc, argv);
    if (result.count("help")) {
        cout << description;
        cout << options.help() << '\n';
        std::exit(0);
    }
    if (result.count("policy") == 0) {
        cout << description
             << "\nFor command-line options, see\n"
             << "central_limit_theorem_exec --help\n";
        std::exit(0);
    }
    return opts(
        result["lambda"].as<double>(),
        result["samples"].as<unsigned>(),
        result["experiments"].as<unsigned>(),
        result["cloud"].as<bool>(),
        result["policy"].as<string>());
}

struct scaled_hist {
    scaled_hist(vector<double> const& vals) {
        auto [min_val, max_val] = std::minmax_element(vals.begin(), vals.end());
        bucket_size = (*max_val - *min_val) / 10;
        for (auto const& val : vals) ++hist[val / bucket_size];
    }
    friend ostream& operator<<(ostream& os, scaled_hist const& sh) {
        size_t constexpr stars = 100;
        auto const total = accumulate(sh.hist.begin(), sh.hist.end(), 0.0,
            [](auto acc, auto kv) { return acc + kv.second; });
        unsigned const val_per_star = total / stars + 1;
        for (auto const& [x, y] : sh.hist) {
            cout << format("{:6.3f}-{:6.3f} {:*>{}}\n",
                x * sh.bucket_size, (x + 1) * sh.bucket_size,
                "", y / val_per_star);
        }
        return os;
    }
    double bucket_size{};
    map<int, unsigned> hist;
};

// Templated runner so we can pass distinct policy types (sequenced_policy,
// parallel_policy, parallel_unsequenced_policy) and a scheduler/callable
// pair without erasing types at runtime.
template <class Scheduler, class Policy, class F>
void run(Scheduler sch, Policy policy, std::size_t n, F&& f) {
    stdexec::sync_wait(
        stdexec::schedule(sch)
        | stdexec::bulk(policy, n, std::forward<F>(f)));
}

int main(int argc, char* argv[]) {
    opts o = get_opts(argc, argv);
    vector<exp_parameters> specification(o.experiments, {o.lambda, o.samples});
    vector<double> means(o.experiments);

    // Local thread-pool scheduler for the seq/par/par_unseq variants.
    // Sized to hardware concurrency by default; bulk's policy refines
    // how fan-out happens within the pool.
    exec::static_thread_pool local_pool;
    auto local_sch = local_pool.get_scheduler();
    auto cloud_sch = lambda_sch.get();

    auto local_proxy = [&](int i) { means[i] = exp_mean(specification[i]); };
    auto cloud_proxy = [&](int i) { means[i] = cloud_exp_mean(specification[i]); };

    if (o.policy == "seq") {
        if (o.cloud) run(cloud_sch, std::execution::seq, specification.size(), cloud_proxy);
        else         run(local_sch, std::execution::seq, specification.size(), local_proxy);
    } else if (o.policy == "par") {
        run(local_sch, std::execution::par, specification.size(), local_proxy);
    } else if (o.policy == "par_unseq") {
        run(local_sch, std::execution::par_unseq, specification.size(), local_proxy);
    } else if (o.policy == "cloud" || o.policy == "cloud_launch") {
        run(cloud_sch, std::execution::par, specification.size(), cloud_proxy);
    } else {
        cerr << format("Invalid execution policy: {}\n", o.policy);
        return 1;
    }

    cout << scaled_hist(means);
    return 0;
}
