#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "gateway/auth.h"
#include "gateway/server.h"
#include "infra/config.h"
#include "infra/status.h"
#include "infra/thread_pool.h"

namespace kvai::gateway {

class HttpGatewayRuntime {
public:
    explicit HttpGatewayRuntime(kvai::infra::ServerConfig config);
    ~HttpGatewayRuntime();

    kvai::infra::Status Start();
    kvai::infra::Status Stop();
    void Wait();

private:
    void AcceptLoop();
    void HandleClient(int client_fd);
    void WakeWaiters();

    kvai::infra::ServerConfig config_;
    ApiKeyAuthenticator authenticator_;
    InProcessGatewayServer server_;
    kvai::infra::ThreadPool http_pool_;
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
    std::mutex wait_mutex_;
    std::condition_variable wait_condition_;
    int listen_fd_ = -1;
    bool started_ = false;
};

}  // namespace kvai::gateway