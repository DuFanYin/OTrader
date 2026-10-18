#pragma once

/**
 * A TWS on a local port, for the IB adapters' tests: it speaks the API's wire protocol (server
 * version 176, text fields; no protobuf), names one managed account, keeps every request the client
 * sends and answers only what a test sends it. A message from it gives its leading fields; the rest
 * are sent empty, which the API decodes as unset.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tests {

class FakeTws {
  public:
    using Fields = std::vector<std::string>;

    /** on_request: called on its thread with each request as it comes, before it is kept (a
     * benchmark stamps its arrival). */
    explicit FakeTws(std::string account = "DU1",
                     std::function<void(const std::vector<std::string>&)> on_request = {})
        : account_(std::move(account)), on_request_(std::move(on_request)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        socklen_t len = sizeof(addr);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 1) != 0 ||
            ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("FakeTws: cannot listen");
        }
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeTws() {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (client_fd_ >= 0) {
            ::shutdown(client_fd_, SHUT_RDWR);
        }
        thread_.join();
        if (client_fd_ >= 0) {
            ::close(client_fd_);
        }
    }
    FakeTws(const FakeTws&) = delete;
    FakeTws& operator=(const FakeTws&) = delete;

    [[nodiscard]] int port() const { return port_; }

    /** A message to the client: its leading fields, then `empty` fields left unset. */
    void send(std::initializer_list<std::string> fields, int empty = 0) {
        std::string body;
        for (const std::string& f : fields) {
            body += f;
            body += '\0';
        }
        body.append(static_cast<std::size_t>(empty), '\0');
        write_frame(body);
    }

    /** The first request with message id `msg_id` from the `from`th on, waiting up to `wait`. */
    auto request(int msg_id, std::size_t from = 0,
                 std::chrono::milliseconds wait = std::chrono::seconds(5)) -> std::optional<Fields> {
        std::unique_lock lock(mutex_);
        std::optional<Fields> found;
        received_.wait_for(lock, wait, [&] {
            for (std::size_t i = from; i < requests_.size(); ++i) {
                if (!requests_[i].empty() && requests_[i][0] == std::to_string(msg_id)) {
                    found = requests_[i];
                    return true;
                }
            }
            return false;
        });
        return found;
    }

    /** How many requests have come so far. */
    auto requests() -> std::size_t {
        std::scoped_lock lock(mutex_);
        return requests_.size();
    }

    // The API's message ids: from TWS, and the requests a test answers.
    static constexpr int kOrderStatus = 3;
    static constexpr int kError = 4;
    static constexpr int kOpenOrder = 5;
    static constexpr int kContractData = 10;
    static constexpr int kContractDataEnd = 52;
    static constexpr int kReqContractData = 9;
    static constexpr int kReqSecDefOptParams = 78;
    static constexpr int kPlaceOrder = 3;

  private:
    static constexpr int kServerVersion = 176;

    void serve() {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        client_fd_ = fd;
        char api[4];
        if (!read_exact(api, sizeof(api)) || std::memcmp(api, "API", 4) != 0 || !read_frame()) {
            return; // the client's versions; it takes ours
        }
        send({std::to_string(kServerVersion), "20261017 12:00:00 UTC"});
        if (!read_frame()) { // startApi
            return;
        }
        send({"9", "1", "1"});        // nextValidId 1
        send({"15", "1", account_}); // managedAccounts
        while (const std::optional<std::string> body = read_frame()) {
            Fields fields;
            std::size_t start = 0;
            for (std::size_t i = 0; i < body->size(); ++i) {
                if ((*body)[i] == '\0') {
                    fields.push_back(body->substr(start, i - start));
                    start = i + 1;
                }
            }
            if (on_request_) {
                on_request_(fields);
            }
            std::scoped_lock lock(mutex_);
            requests_.push_back(std::move(fields));
            received_.notify_all();
        }
    }

    auto read_exact(char* out, std::size_t n) -> bool {
        while (n > 0) {
            const ssize_t got = ::recv(client_fd_, out, n, 0);
            if (got <= 0) {
                return false;
            }
            out += got;
            n -= static_cast<std::size_t>(got);
        }
        return true;
    }

    auto read_frame() -> std::optional<std::string> {
        std::uint32_t len = 0;
        if (!read_exact(reinterpret_cast<char*>(&len), sizeof(len))) {
            return std::nullopt;
        }
        std::string body(ntohl(len), '\0');
        if (!read_exact(body.data(), body.size())) {
            return std::nullopt;
        }
        return body;
    }

    void write_frame(const std::string& body) {
        std::scoped_lock lock(write_mutex_);
        const std::uint32_t len = htonl(static_cast<std::uint32_t>(body.size()));
        std::string frame(reinterpret_cast<const char*>(&len), sizeof(len));
        frame += body;
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const ssize_t n = ::send(client_fd_, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    std::string account_;
    std::function<void(const std::vector<std::string>&)> on_request_;
    int listen_fd_ = -1;
    std::atomic<int> client_fd_{-1};
    int port_ = 0;
    std::mutex mutex_;
    std::mutex write_mutex_;
    std::condition_variable received_;
    std::vector<Fields> requests_;
    std::thread thread_;
};

} // namespace tests
