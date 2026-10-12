#pragma once

/**
 * IbSession: one API connection to IB Gateway / TWS, the base of the order gateway and of the
 * market data feed, each on a connection (and client id) of its own. It connects, runs a thread
 * that hands each message to the EWrapper callbacks as it arrives, and reconnects after a drop.
 * Requests go through send(), which serializes them: the TWS socket is not thread-safe. What must
 * not wait on IB (the engine's orders and cancels) is queued for the message thread to send, on a
 * lock-free queue.
 *
 * A derived class calls stop() in its destructor, since the thread calls its hooks.
 */

#include "../../utilities/base_engine.hpp"
#include "../../utilities/log_level.hpp"
#include "../../utilities/mpsc_queue.hpp"

#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace adapters {

class IbSession : public DefaultEWrapper {
  public:
    /** owner: the engine the session works for; its lines go under the owner's name. */
    explicit IbSession(const utilities::BaseEngine& owner);
    ~IbSession() override;

    IbSession(const IbSession&) = delete;
    IbSession& operator=(const IbSession&) = delete;

    /** Connect and start the message thread; false when IB cannot be reached. From then on a
     * dropped connection is retried every few seconds, until stop(). */
    bool start(const std::string& host, int port, int client_id);
    void stop();
    [[nodiscard]] bool connected() const { return connected_; }

  protected:
    /** Run request(EClientSocket&) while connected; false if not. Any thread. */
    template <typename Request> bool send(Request&& request) {
        std::scoped_lock lock(send_mutex_);
        if (!connected_) {
            return false;
        }
        std::forward<Request>(request)(socket_);
        return true;
    }

    /** Run job on the message thread, after the jobs queued before it, so the caller never
     * waits on IB's socket. The jobs still queued when the session stops run as it stops (their
     * send() fails). Any thread. */
    void queue_on_message_thread(std::function<void()> job);

    /** After each connect, before its first message: on start()'s caller the first time, then on
     * the message thread. */
    virtual void on_connected() {}
    /** The connection dropped (message thread); it is being retried. */
    virtual void on_disconnected() {}
    /** After a reconnect, once on_connected has run (message thread). */
    virtual void on_reconnected() {}
    /** After each batch of messages, and at least every 100 ms while connected (message
     * thread). */
    virtual void on_wake() {}
    /** An error about one request (id: its order or request id); the default logs it. */
    virtual void on_error(int id, int code, const std::string& message);

    [[nodiscard]] const utilities::BaseEngine& owner() const { return owner_; }
    void log(const std::string& message, utilities::LogLevel level) const { owner_.write_log(message, level); }

  private:
    void error(int id, long long errorTimeMs, int errorCode, const std::string& errorString,
               const std::string& advancedOrderRejectJson) override;
    void connectionClosed() override;

    bool open();
    void close();
    void run(const std::stop_token& st);
    /** The jobs queued so far, in order (message thread, or stop()). */
    void run_queued_jobs();

    /** A job in the message thread's queue. */
    struct Job {
        std::function<void()> run;
        std::atomic<Job*> next{nullptr};
    };

    const utilities::BaseEngine& owner_;
    EReaderOSSignal signal_;
    EClientSocket socket_;
    std::unique_ptr<EReader> reader_;
    std::mutex send_mutex_;
    utilities::MpscQueue<Job> jobs_; // queue_on_message_thread's
    std::atomic<bool> connected_{false};
    std::atomic<bool> reconnecting_{false};
    std::string host_;
    int port_ = 0;
    int client_id_ = 0;
    std::jthread thread_;
};

} // namespace adapters
