/** IbSession: connect, hand messages over as they arrive, reconnect. */

#include "ib_session.hpp"
#include "../../utilities/log_level.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <memory>

namespace adapters {

namespace {

using namespace std::chrono_literals;

constexpr auto kWake = 100ms; // the longest the message thread waits for a message
constexpr auto kRetry = 5s;   // between attempts to reconnect, and from a reconnect to the next

// About IB's own connections (data farms and the like), and the end of an account subscription
// the gateway asked for (2100): news, not errors.
constexpr std::array kNotices{1102, 2100, 2104, 2106, 2107, 2108, 2119, 2158};

/** How loud a message about the connection is: news, a warning (lost connectivity, 21xx) or an
 * error. */
auto connection_message_level(int code) -> utilities::LogLevel {
    if (std::ranges::contains(kNotices, code)) {
        return utilities::INFO;
    }
    if (code == 1100 || (code >= 2100 && code < 2200)) {
        return utilities::WARNING;
    }
    return utilities::ERROR;
}

} // namespace

IbSession::IbSession(const utilities::BaseEngine& owner)
    : owner_(owner), signal_(static_cast<unsigned long>(kWake.count())), socket_(this, &signal_) {}

IbSession::~IbSession() { stop(); }

auto IbSession::start(const std::string& host, int port, int client_id) -> bool {
    stop();
    host_ = host;
    port_ = port;
    client_id_ = client_id;
    reconnecting_ = false;
    if (!open()) {
        return false;
    }
    on_connected();
    thread_ = std::jthread([this](const std::stop_token& st) { run(st); });
    return true;
}

void IbSession::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    close();
    run_queued_jobs(); // not connected: their sends fail, and say so
}

void IbSession::queue_on_message_thread(std::function<void()> job) {
    jobs_.push(new Job{.run = std::move(job)}); // NOLINT(cppcoreguidelines-owning-memory)
    signal_.issueSignal(); // the message thread stops waiting for IB
}

void IbSession::run_queued_jobs() {
    // A job pushed meanwhile but not linked yet comes out at the next run: its signal wakes it.
    while (Job* job = jobs_.pop()) {
        const std::unique_ptr<Job> done(job);
        done->run();
    }
}

auto IbSession::open() -> bool {
    std::scoped_lock lock(send_mutex_);
    // TWS paces a burst of requests (over 50 a second) instead of dropping the connection.
    socket_.setConnectOptions("+PACEAPI");
    if (!socket_.eConnect(host_.c_str(), port_, client_id_)) {
        return false;
    }
    reader_ = std::make_unique<EReader>(&socket_, &signal_);
    reader_->start();
    connected_ = true;
    return true;
}

void IbSession::close() {
    std::scoped_lock lock(send_mutex_);
    connected_ = false;
    // The TWS API's reader registers with the socket and never unregisters, and the socket's
    // eDisconnect joins the registered reader's thread: unregistered first, the reader's destructor
    // joins its thread once, and the eDisconnect after it touches no freed reader.
    socket_.registerEReader(nullptr);
    reader_.reset();
    socket_.eDisconnect();
}

void IbSession::run(const std::stop_token& st) {
    std::chrono::steady_clock::time_point retry_at{};
    while (!st.stop_requested()) {
        if (connected_ && socket_.isConnected()) {
            signal_.waitForSignal();
            run_queued_jobs(); // before IB's messages: an order booked before its answers come
            reader_->processMsgs();
            run_queued_jobs();
            on_wake();
            continue;
        }
        run_queued_jobs(); // not connected: their sends fail, and say so
        if (!reconnecting_) {
            if (reader_) {
                reader_->processMsgs(); // what IB said before it closed, its reason among it
            }
            close();
            reconnecting_ = true;
            log(std::format("IB connection {}:{} lost; reconnecting", host_, port_), utilities::WARNING);
            on_disconnected();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < retry_at) {
            std::this_thread::sleep_for(kWake);
        } else if (open()) {
            reconnecting_ = false;
            retry_at = now + kRetry; // if it drops at once (IB Gateway restarting), the next waits
            log(std::format("IB connection {}:{} restored", host_, port_), utilities::INFO);
            on_connected();
            on_reconnected();
        } else {
            retry_at = now + kRetry;
        }
    }
}

void IbSession::error(int id, long long /*errorTimeMs*/, int errorCode,
                      const std::string& errorString,
                      const std::string& /*advancedOrderRejectJson*/) {
    if (id >= 0) { // about a request; -1 is about the connection
        on_error(id, errorCode, errorString);
        return;
    }
    if (errorCode == 502 && reconnecting_) {
        return; // another failed attempt to reconnect: run() said it is reconnecting
    }
    log(std::format("IB {}: {}", errorCode, errorString), connection_message_level(errorCode));
}

void IbSession::on_error(int id, int code, const std::string& message) {
    log(std::format("IB {} (request {}): {}", code, id, message), utilities::ERROR);
}

void IbSession::connectionClosed() {
    connected_ = false; // the message thread notices, says so and reconnects
}

} // namespace adapters
