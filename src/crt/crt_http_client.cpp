/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

// CRT-based Aws::Http::HttpClient implementation.
//
// Replaces the SDK's default libcurl-easy-based CurlHttpClient (whose
// CurlHandleContainer mutex caps single-client concurrency at ~50 even with
// 1000-thread executors) with a thin wrapper over aws-c-http's
// connection_manager. The connection manager runs on the CRT event loop
// group's epoll threads and uses non-blocking I/O for all connection
// management, eliminating the per-handle mutex contention.
//
// The Aws::Http::HttpClient::MakeRequest interface is synchronous from the
// caller's perspective (the SDK requires that), so each call blocks the
// calling thread on a std::future while the I/O happens on the CRT event
// loop. The win isn't that any single MakeRequest is faster — it's that
// hundreds of concurrent MakeRequests don't pile up on a shared mutex.

#include "awslabs/enhanced/crt/crt_http_client.h"

#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/URI.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/memory/AWSMemory.h>

#include <aws/common/allocator.h>
#include <aws/common/byte_buf.h>
#include <aws/cal/cal.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/socket.h>
#include <aws/io/stream.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/http/http.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>

#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

aws_byte_cursor CursorFromString(const Aws::String& s) {
    aws_byte_cursor c;
    c.ptr = reinterpret_cast<uint8_t*>(const_cast<char*>(s.data()));
    c.len = s.size();
    return c;
}

aws_byte_cursor CursorFromStdString(const std::string& s) {
    aws_byte_cursor c;
    c.ptr = reinterpret_cast<uint8_t*>(const_cast<char*>(s.data()));
    c.len = s.size();
    return c;
}

std::string StringFromCursor(const aws_byte_cursor& c) {
    return std::string(reinterpret_cast<const char*>(c.ptr), c.len);
}

const char* HttpMethodName(Aws::Http::HttpMethod method) {
    switch (method) {
        case Aws::Http::HttpMethod::HTTP_GET:    return "GET";
        case Aws::Http::HttpMethod::HTTP_POST:   return "POST";
        case Aws::Http::HttpMethod::HTTP_PUT:    return "PUT";
        case Aws::Http::HttpMethod::HTTP_DELETE: return "DELETE";
        case Aws::Http::HttpMethod::HTTP_HEAD:   return "HEAD";
        case Aws::Http::HttpMethod::HTTP_PATCH:  return "PATCH";
    }
    return "GET";
}

// ---------------------------------------------------------------------------
// CrtSharedState — process-wide singleton holding the CRT runtime objects
// that should be created once and shared across every CrtHttpClient.
// ---------------------------------------------------------------------------
//
// Held by shared_ptr inside CrtHttpClientFactory; CrtHttpClient instances
// hold their own shared_ptr so the state outlives any client that references
// it. We intentionally do NOT release the CRT libraries at process exit:
// the CRT's shutdown is async and ordering-sensitive, and we'd rather leak
// at exit than risk use-after-free during global destruction of an AWS SDK
// that may still be running other threads.

class CrtSharedState {
public:
    CrtSharedState(uint16_t event_loop_threads)
        : alloc_(aws_default_allocator()) {
        // Library init order matters: common -> cal -> io -> http.
        // aws-c-common is brought in transitively by every aws-c-* library
        // and initializes itself on first use, so we don't init it here.
        aws_cal_library_init(alloc_);
        aws_io_library_init(alloc_);
        aws_http_library_init(alloc_);

        // Event loop group: one epoll thread per "slot". 0 = hardware
        // concurrency. These threads drive all socket I/O and timers.
        aws_event_loop_group_options elg_opts{};
        elg_opts.loop_count = event_loop_threads;
        elg_ = aws_event_loop_group_new(alloc_, &elg_opts);

        // Host resolver — caches DNS lookups across requests so that 500
        // simultaneous Lambda invocations don't trigger 500 DNS queries.
        aws_host_resolver_default_options hr_opts{};
        hr_opts.el_group = elg_;
        hr_opts.max_entries = 16;
        host_resolver_ = aws_host_resolver_new_default(alloc_, &hr_opts);

        // Client bootstrap — bundles the event loop group + host resolver
        // and is what connection managers actually configure against.
        aws_client_bootstrap_options bs_opts{};
        bs_opts.event_loop_group = elg_;
        bs_opts.host_resolver = host_resolver_;
        client_bootstrap_ = aws_client_bootstrap_new(alloc_, &bs_opts);

        // TLS context — one client TLS context shared by every connection
        // manager we create. Default verification (use the system CA bundle).
        aws_tls_ctx_options tls_opts{};
        aws_tls_ctx_options_init_default_client(&tls_opts, alloc_);
        tls_ctx_ = aws_tls_client_ctx_new(alloc_, &tls_opts);
        aws_tls_ctx_options_clean_up(&tls_opts);
    }

    ~CrtSharedState() {
        // Release in reverse-init order. This must run BEFORE
        // Aws::CleanupCrt() (which calls aws_thread_join_all_managed and
        // would otherwise wait forever for our still-live threads). The SDK
        // calls CleanupHttp -> releases the global factory shared_ptr ->
        // drops our last ref -> we destruct here -> CRT threads exit, then
        // CleanupCrt runs and completes immediately.
        if (tls_ctx_)          aws_tls_ctx_release(tls_ctx_);
        if (client_bootstrap_) aws_client_bootstrap_release(client_bootstrap_);
        if (host_resolver_)    aws_host_resolver_release(host_resolver_);
        if (elg_)              aws_event_loop_group_release(elg_);
    }

    aws_allocator*         alloc()             const { return alloc_; }
    aws_client_bootstrap*  client_bootstrap()  const { return client_bootstrap_; }
    aws_tls_ctx*           tls_ctx()           const { return tls_ctx_; }

private:
    aws_allocator*         alloc_              = nullptr;
    aws_event_loop_group*  elg_                = nullptr;
    aws_host_resolver*     host_resolver_      = nullptr;
    aws_client_bootstrap*  client_bootstrap_   = nullptr;
    aws_tls_ctx*           tls_ctx_            = nullptr;
};

// ---------------------------------------------------------------------------
// PerRequestState — one of these lives for the duration of one MakeRequest.
// Owns the response object being assembled, the promise the caller awaits,
// and the CRT objects whose lifetime must outlast the request callbacks.
// ---------------------------------------------------------------------------

struct PerRequestState {
    // The caller-visible response, populated incrementally from CRT callbacks.
    std::shared_ptr<Aws::Http::HttpResponse> response;

    // Set by on_complete; MakeRequest's calling thread blocks on the future.
    std::promise<std::shared_ptr<Aws::Http::HttpResponse>> promise;

    // The CRT message we built (host headers, method, path, body cursor).
    aws_http_message* request_message = nullptr;

    // Body bytes copied out of the SDK's request body stream into a flat
    // buffer (since aws_input_stream_new_from_cursor needs a stable pointer).
    std::string flattened_body;
    aws_input_stream* body_stream = nullptr;

    // The connection we acquired from the manager (must be released back).
    aws_http_connection_manager* owning_manager = nullptr;
    aws_http_connection* connection = nullptr;
};

// ---------------------------------------------------------------------------
// ConnectionManagerCache — one aws_http_connection_manager per (host, port).
// Lazy-created on first request to that host, kept alive for the process.
// ---------------------------------------------------------------------------

class ConnectionManagerCache {
public:
    explicit ConnectionManagerCache(std::shared_ptr<CrtSharedState> shared)
        : shared_(std::move(shared)) {}

    ~ConnectionManagerCache() {
        // Release each manager. Shutdown is async (the CRT will spin down
        // its connections on the event loop), but the manager holds its
        // own ref to the client_bootstrap, so the bootstrap stays alive
        // until the manager finishes. Our CrtSharedState destructor runs
        // after this one and releases the bootstrap; CRT ref-counting
        // sorts out the actual destruction order.
        std::lock_guard<std::mutex> g(mu_);
        for (auto& [_, mgr] : managers_) {
            aws_http_connection_manager_release(mgr);
        }
        managers_.clear();
    }

    // Get or lazily create a connection manager for host:port. Returns a
    // raw pointer; the cache owns the lifetime.
    aws_http_connection_manager* Get(const std::string& host, uint16_t port,
                                     size_t max_connections) {
        std::string key = host + ":" + std::to_string(port);
        std::lock_guard<std::mutex> g(mu_);
        auto it = managers_.find(key);
        if (it != managers_.end()) return it->second;

        // First time we see this host — create a manager.
        aws_socket_options socket_opts{};
        socket_opts.type = AWS_SOCKET_STREAM;
        socket_opts.domain = AWS_SOCKET_IPV4;
        socket_opts.connect_timeout_ms = 5000;

        // Per-manager TLS connection options derived from the shared ctx.
        // The SNI server_name must be set to the host we're talking to.
        aws_tls_connection_options tls_conn_opts{};
        aws_tls_connection_options_init_from_ctx(&tls_conn_opts, shared_->tls_ctx());
        aws_byte_cursor host_cursor = CursorFromStdString(host);
        aws_tls_connection_options_set_server_name(&tls_conn_opts,
                                                   shared_->alloc(),
                                                   &host_cursor);

        aws_http_connection_manager_options cm_opts{};
        cm_opts.bootstrap = shared_->client_bootstrap();
        cm_opts.socket_options = &socket_opts;
        cm_opts.tls_connection_options = &tls_conn_opts;
        cm_opts.host = host_cursor;
        cm_opts.port = port;
        cm_opts.max_connections = max_connections;
        cm_opts.max_pending_connection_acquisitions = max_connections * 4;
        cm_opts.connection_acquisition_timeout_ms = 60000;
        // No explicit shutdown callback: we leak managers at process exit.

        aws_http_connection_manager* mgr = aws_http_connection_manager_new(
            shared_->alloc(), &cm_opts);
        aws_tls_connection_options_clean_up(&tls_conn_opts);

        if (mgr) managers_.emplace(std::move(key), mgr);
        return mgr;
    }

private:
    std::shared_ptr<CrtSharedState> shared_;
    std::mutex mu_;  // Only contended on first-touch of each host. Cold path.
    std::map<std::string, aws_http_connection_manager*> managers_;
};

// ---------------------------------------------------------------------------
// CrtHttpClient — the SDK-facing class. Translates Aws::Http::HttpRequest
// into a CRT request, submits via the connection manager, and blocks the
// calling thread on the std::future until on_complete fires.
// ---------------------------------------------------------------------------

class CrtHttpClient : public Aws::Http::HttpClient {
public:
    CrtHttpClient(std::shared_ptr<CrtSharedState> shared,
                  std::shared_ptr<ConnectionManagerCache> cache,
                  size_t max_connections_per_host)
        : shared_(std::move(shared)),
          cm_cache_(std::move(cache)),
          max_connections_per_host_(max_connections_per_host) {}

    std::shared_ptr<Aws::Http::HttpResponse> MakeRequest(
        const std::shared_ptr<Aws::Http::HttpRequest>& request,
        Aws::Utils::RateLimits::RateLimiterInterface* /*readLimiter*/  = nullptr,
        Aws::Utils::RateLimits::RateLimiterInterface* /*writeLimiter*/ = nullptr)
        const override
    {
        // Build the SDK-visible response shell up front so we can populate
        // it from the CRT callbacks before fulfilling the promise.
        auto response = Aws::MakeShared<Aws::Http::Standard::StandardHttpResponse>(
            "CrtHttpClient", request);

        auto state = std::make_unique<PerRequestState>();
        state->response = response;

        const auto& uri = request->GetUri();
        std::string host = uri.GetAuthority().c_str();
        uint16_t port = uri.GetPort();
        if (port == 0) {
            port = (uri.GetScheme() == Aws::Http::Scheme::HTTPS) ? 443 : 80;
        }

        aws_http_connection_manager* mgr =
            cm_cache_->Get(host, port, max_connections_per_host_);
        if (!mgr) {
            // Connection manager construction failed; return a synthetic
            // 5xx response so the SDK retry logic kicks in.
            response->SetResponseCode(
                Aws::Http::HttpResponseCode::INTERNAL_SERVER_ERROR);
            return response;
        }
        state->owning_manager = mgr;

        // Translate the SDK request into a CRT message.
        aws_http_message* msg = aws_http_message_new_request(shared_->alloc());
        state->request_message = msg;

        const char* method = HttpMethodName(request->GetMethod());
        aws_byte_cursor method_cur = aws_byte_cursor_from_c_str(method);
        aws_http_message_set_request_method(msg, method_cur);

        // The CRT wants path + query (the ":path" pseudo-header equivalent).
        // The SDK's URI splits these; reassemble.
        std::string path_and_query = uri.GetURLEncodedPath().c_str();
        if (path_and_query.empty()) path_and_query = "/";
        const auto& qs = uri.GetQueryString();
        if (!qs.empty()) path_and_query += qs.c_str();
        aws_byte_cursor path_cur = CursorFromStdString(path_and_query);
        aws_http_message_set_request_path(msg, path_cur);

        // Translate headers. The SDK has already signed the request, so we
        // ship its headers verbatim. We *must* add a Host header explicitly
        // even though it's not always set on the SDK side, because the CRT
        // message has no concept of an implicit host.
        bool saw_host_header = false;
        for (auto const& kv : request->GetHeaders()) {
            aws_http_header h{};
            h.name  = CursorFromString(kv.first);
            h.value = CursorFromString(kv.second);
            aws_http_message_add_header(msg, h);
            if (kv.first == "host" || kv.first == "Host") saw_host_header = true;
        }
        if (!saw_host_header) {
            aws_http_header h{};
            h.name  = aws_byte_cursor_from_c_str("Host");
            h.value = CursorFromStdString(host);
            aws_http_message_add_header(msg, h);
        }

        // Body: flatten the SDK's iostream into a std::string so we can
        // hand the CRT a stable byte_cursor pointer for its input stream.
        // Lambda request bodies are tiny JSON; cost is negligible.
        auto body = request->GetContentBody();
        if (body) {
            body->seekg(0, std::ios_base::end);
            auto end = body->tellg();
            body->seekg(0, std::ios_base::beg);
            if (end > 0) {
                state->flattened_body.resize(static_cast<size_t>(end));
                body->read(state->flattened_body.data(),
                           static_cast<std::streamsize>(end));
                state->flattened_body.resize(
                    static_cast<size_t>(body->gcount()));
                aws_byte_cursor body_cur =
                    CursorFromStdString(state->flattened_body);
                state->body_stream = aws_input_stream_new_from_cursor(
                    shared_->alloc(), &body_cur);
                aws_http_message_set_body_stream(msg, state->body_stream);
            }
        }

        // Hand off to async land. The PerRequestState is owned by `state`
        // here; we leak it to the callback chain and reclaim it in
        // OnStreamComplete.
        std::future<std::shared_ptr<Aws::Http::HttpResponse>> fut =
            state->promise.get_future();
        PerRequestState* raw_state = state.release();

        aws_http_connection_manager_acquire_connection(
            mgr, &CrtHttpClient::OnConnectionAcquired, raw_state);

        // Block calling thread on response (the entire point of this class
        // is that this block happens on the caller's thread, NOT on a
        // shared mutex held by the SDK's curl handle pool).
        return fut.get();
    }

private:
    // -- CRT callback chain --------------------------------------------------

    static void OnConnectionAcquired(aws_http_connection* connection,
                                     int error_code, void* user_data) {
        auto* st = static_cast<PerRequestState*>(user_data);
        if (error_code || !connection) {
            st->response->SetResponseCode(
                Aws::Http::HttpResponseCode::INTERNAL_SERVER_ERROR);
            st->promise.set_value(st->response);
            CleanUp(st);
            return;
        }
        st->connection = connection;

        aws_http_make_request_options req_opts{};
        req_opts.self_size = sizeof(aws_http_make_request_options);
        req_opts.request = st->request_message;
        req_opts.user_data = st;
        req_opts.on_response_headers          = &CrtHttpClient::OnResponseHeaders;
        req_opts.on_response_header_block_done = nullptr;
        req_opts.on_response_body             = &CrtHttpClient::OnResponseBody;
        req_opts.on_complete                  = &CrtHttpClient::OnStreamComplete;

        aws_http_stream* stream =
            aws_http_connection_make_request(connection, &req_opts);
        if (!stream || aws_http_stream_activate(stream) != AWS_OP_SUCCESS) {
            st->response->SetResponseCode(
                Aws::Http::HttpResponseCode::INTERNAL_SERVER_ERROR);
            st->promise.set_value(st->response);
            if (stream) aws_http_stream_release(stream);
            CleanUp(st);
        }
        // On success: stream is alive; on_complete will fire and call
        // CleanUp once the response is done.
    }

    static int OnResponseHeaders(aws_http_stream* /*stream*/,
                                 enum aws_http_header_block /*block*/,
                                 const aws_http_header* header_array,
                                 size_t num_headers, void* user_data) {
        auto* st = static_cast<PerRequestState*>(user_data);
        for (size_t i = 0; i < num_headers; ++i) {
            st->response->AddHeader(StringFromCursor(header_array[i].name).c_str(),
                                    StringFromCursor(header_array[i].value).c_str());
        }
        return AWS_OP_SUCCESS;
    }

    static int OnResponseBody(aws_http_stream* /*stream*/,
                              const aws_byte_cursor* data, void* user_data) {
        auto* st = static_cast<PerRequestState*>(user_data);
        st->response->GetResponseBody().write(
            reinterpret_cast<const char*>(data->ptr),
            static_cast<std::streamsize>(data->len));
        return AWS_OP_SUCCESS;
    }

    static void OnStreamComplete(aws_http_stream* stream, int error_code,
                                 void* user_data) {
        auto* st = static_cast<PerRequestState*>(user_data);
        if (error_code == AWS_ERROR_SUCCESS) {
            int status = 0;
            aws_http_stream_get_incoming_response_status(stream, &status);
            st->response->SetResponseCode(
                static_cast<Aws::Http::HttpResponseCode>(status));
        } else {
            st->response->SetResponseCode(
                Aws::Http::HttpResponseCode::INTERNAL_SERVER_ERROR);
        }
        st->promise.set_value(st->response);
        if (stream) aws_http_stream_release(stream);
        CleanUp(st);
    }

    static void CleanUp(PerRequestState* st) {
        if (st->connection && st->owning_manager) {
            aws_http_connection_manager_release_connection(st->owning_manager,
                                                           st->connection);
        }
        if (st->request_message) aws_http_message_release(st->request_message);
        if (st->body_stream)     aws_input_stream_release(st->body_stream);
        delete st;
    }

    std::shared_ptr<CrtSharedState>        shared_;
    std::shared_ptr<ConnectionManagerCache> cm_cache_;
    size_t                                 max_connections_per_host_;
};

// ---------------------------------------------------------------------------
// CrtHttpClientFactory — what the SDK actually asks for clients from.
// CreateHttpRequest just produces a standard SDK request object; only the
// CreateHttpClient call returns our CRT-backed implementation.
// ---------------------------------------------------------------------------

class CrtHttpClientFactory : public Aws::Http::HttpClientFactory {
public:
    CrtHttpClientFactory(std::shared_ptr<CrtSharedState> shared,
                         std::shared_ptr<ConnectionManagerCache> cache)
        : shared_(std::move(shared)), cache_(std::move(cache)) {}

    std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(
        const Aws::Client::ClientConfiguration& config) const override {
        size_t max_conns = config.maxConnections > 0
            ? static_cast<size_t>(config.maxConnections)
            : 1000;
        return Aws::MakeShared<CrtHttpClient>(
            "CrtHttpClient", shared_, cache_, max_conns);
    }

    std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(
        const Aws::String& uri, Aws::Http::HttpMethod method,
        const Aws::IOStreamFactory& streamFactory) const override {
        return CreateHttpRequest(Aws::Http::URI(uri), method, streamFactory);
    }

    std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(
        const Aws::Http::URI& uri, Aws::Http::HttpMethod method,
        const Aws::IOStreamFactory& streamFactory) const override {
        auto req = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
            "CrtHttpClient", uri, method);
        req->SetResponseStreamFactory(streamFactory);
        return req;
    }

private:
    std::shared_ptr<CrtSharedState>        shared_;
    std::shared_ptr<ConnectionManagerCache> cache_;
};

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry point.
// ---------------------------------------------------------------------------

namespace AwsLabs::Enhanced::Crt {

void InstallCrtHttpFactory(uint16_t eventLoopThreads) {
    static std::once_flag installed;
    std::call_once(installed, [eventLoopThreads]() {
        auto shared = std::make_shared<CrtSharedState>(eventLoopThreads);
        auto cache  = std::make_shared<ConnectionManagerCache>(shared);
        auto factory = std::make_shared<CrtHttpClientFactory>(shared, cache);
        Aws::Http::SetHttpClientFactory(factory);
    });
}

}  // namespace AwsLabs::Enhanced::Crt
