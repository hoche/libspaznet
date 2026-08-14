#pragma once

// ResponseWriter<Response>: a movable/copyable completion token that lets a
// request handler answer either synchronously (call complete() before
// returning) or later, from anywhere (move/copy the writer somewhere
// durable — a lambda, a member, a queue — and call complete() from a
// callback, a different thread, or a coroutine that outlives the original
// call). Exactly one complete() call across all copies takes effect; the
// rest are silently ignored, so a handler racing a timeout against real
// work never double-answers.
//
// This is the runtime-neutral half of the handler API: it has no
// coroutine dependency and works identically whether the caller is a
// coroutine dispatcher (see the awaiter bridge in
// example/http/src/dispatcher_coroutine.cpp), a future reactor state machine polling
// is_completed(), or a plain synchronous caller that always completes
// inline. on_ready() is the generic hook either of the latter two build on.

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace spaznet {

template <typename Response>
class ResponseWriter {
  public:
    // `deliver` is the dispatcher's "do something with the finished
    // response" action (e.g. serialize + write to the wire). It runs
    // exactly once, synchronously inside whichever complete() call wins the
    // race, on whichever thread makes that call.
    explicit ResponseWriter(std::function<void(Response)> deliver)
        : state_(std::make_shared<State>(std::move(deliver))) {}

    ResponseWriter(const ResponseWriter&) = default;
    ResponseWriter(ResponseWriter&&) = default;
    auto operator=(const ResponseWriter&) -> ResponseWriter& = default;
    auto operator=(ResponseWriter&&) -> ResponseWriter& = default;

    // Completes the response. The first call (across this writer and any
    // copies of it) wins and runs `deliver`; every later call is a no-op.
    void complete(Response response) const {
        std::function<void()> ready;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            if (state_->completed) {
                return;
            }
            state_->completed = true;
            state_->deliver(std::move(response));
            ready = std::move(state_->on_ready);
        }
        if (ready) {
            ready();
        }
    }

    auto is_completed() const -> bool {
        std::lock_guard<std::mutex> lock(state_->mu);
        return state_->completed;
    }

    // Registers a callback fired once complete() has run — immediately,
    // inline, if it already has by the time this is called. At most one
    // callback may be registered per writer identity (i.e. per underlying
    // State; copies share it), which is what the codebase needs: one
    // dispatcher awaiting one eventual response.
    void on_ready(std::function<void()> callback) const {
        bool already;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            already = state_->completed;
            if (!already) {
                state_->on_ready = std::move(callback);
            }
        }
        if (already && callback) {
            callback();
        }
    }

  private:
    struct State {
        explicit State(std::function<void(Response)> d) : deliver(std::move(d)) {}
        std::mutex mu;
        std::function<void(Response)> deliver;
        std::function<void()> on_ready;
        bool completed = false;
    };

    std::shared_ptr<State> state_;
};

} // namespace spaznet
