// Broadcast chat-room WebSocket demo (with an HTTP/1.1 fallback for plain
// requests on the same port).
//
//   $ ./ws_chat
//   # then open http://localhost:8080/ in two or more browser tabs, or:
//   $ wscat -c ws://localhost:8080/
//   > hello
//   < * user5 joined (2 online)
//   < user5: hello
//
// The HTTP fallback on the same port serves a tiny self-contained HTML +
// JavaScript chat client (see kChatPage below), so the whole demo can be
// driven from a web browser with no extra tooling.
//
// Unlike demo/echo.cpp, this handler keeps state *across* connections: a
// text message from one client is fanned out to every other connected
// client. That cross-connection fan-out is the interesting part of this
// example — see the concurrency notes below.
//
// Concurrency model
// ------------------
// `Socket` is owned by the per-connection coroutine (see
// src/dispatcher.cpp's serve_websocket) for the life of the connection, and
// is not copyable. If handle_message on connection A tried to call
// send_message() directly on connection B's socket, two application
// coroutines could end up writing the same fd concurrently (a data race on
// the IOContext's per-fd write registration), and B could disconnect
// mid-write (use-after-free on the Socket object).
//
// To avoid that, every connection remains the *sole* writer of its own
// socket. A broadcast is delivered as data — pushed onto the target
// connection's own outbound queue — and a per-connection writer_loop
// coroutine (started from on_open, running independently of the reader
// loop inside serve_websocket) drains that queue and performs the actual
// send_message() calls. The queue is guarded by a plain mutex that is
// never held across a co_await.
//
// Known caveat: serve_websocket still writes its own protocol control
// frames (Pong replies, the closing Close frame) directly from the
// connection's *reader* coroutine, not through writer_loop. In steady
// state that never overlaps an application write to the same socket, but
// a client Ping arriving in the same instant as a broadcast write is a
// narrow, unaddressed interleave. Removing it would require refactoring
// serve_websocket so every write — protocol and application — funnels
// through one place; out of scope for this demo.

#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/websocket/dispatcher.hpp>
#include <libspaznet/websocket/handler.hpp>
#include <libspaznet/websocket/send.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Minimal browser client: connects back to this same host/port over
// WebSocket, appends every received text frame to the log, and sends the
// input box on submit. Kept deliberately dependency-free.
constexpr char kChatPage[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>spaznet WebSocket chat</title>
<style>
  body { font-family: system-ui, sans-serif; max-width: 640px; margin: 2rem auto; padding: 0 1rem; }
  #log { border: 1px solid #ccc; border-radius: 6px; height: 320px; overflow-y: auto;
         padding: 0.5rem; background: #fafafa; white-space: pre-wrap; }
  #log .sys { color: #888; font-style: italic; }
  form { display: flex; gap: 0.5rem; margin-top: 0.75rem; }
  input[type=text] { flex: 1; padding: 0.5rem; }
  button { padding: 0.5rem 1rem; }
  #status { font-size: 0.85rem; color: #666; margin-bottom: 0.5rem; }
</style>
</head>
<body>
<h1>spaznet chat</h1>
<div id="status">connecting&hellip;</div>
<div id="log"></div>
<form id="form" autocomplete="off">
  <input id="input" type="text" placeholder="Type a message and press Enter" disabled>
  <button id="send" type="submit" disabled>Send</button>
</form>
<script>
(function () {
  const log = document.getElementById('log');
  const status = document.getElementById('status');
  const input = document.getElementById('input');
  const send = document.getElementById('send');
  const form = document.getElementById('form');

  function append(text, cls) {
    const line = document.createElement('div');
    if (cls) line.className = cls;
    line.textContent = text;
    log.appendChild(line);
    log.scrollTop = log.scrollHeight;
  }

  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(proto + '//' + location.host + '/');

  ws.onopen = function () {
    status.textContent = 'connected';
    input.disabled = false;
    send.disabled = false;
    input.focus();
  };
  ws.onmessage = function (ev) {
    const sys = ev.data.startsWith('*');
    append(ev.data.replace(/\n+$/, ''), sys ? 'sys' : null);
  };
  ws.onclose = function () {
    status.textContent = 'disconnected';
    input.disabled = true;
    send.disabled = true;
  };
  ws.onerror = function () { status.textContent = 'connection error'; };

  form.addEventListener('submit', function (ev) {
    ev.preventDefault();
    const text = input.value;
    if (text && ws.readyState === WebSocket.OPEN) {
      ws.send(text);
      append('you: ' + text);
      input.value = '';
    }
  });
})();
</script>
</body>
</html>
)HTML";

class HttpFallback : public spaznet::http::HTTPHandler {
  public:
    spaznet::Task handle_request(const spaznet::http::HTTPRequest&,
                                 spaznet::http::HTTPResponse& resp,
                                 spaznet::Socket&) override {
        resp.status_code = 200;
        resp.reason_phrase = "OK";
        resp.set_header("Content-Type", "text/html; charset=utf-8");
        // -1 to drop the trailing NUL of the string literal.
        resp.body.assign(kChatPage, kChatPage + sizeof(kChatPage) - 1);
        co_return;
    }
};

// Per-connection state: an outbound queue fed by *other* connections'
// handle_message calls, drained only by this connection's own writer_loop.
struct Session {
    Session(int id_param, spaznet::Socket* sock_param) : id(id_param), sock(sock_param) {}

    const int id;
    spaznet::Socket* const sock;

    std::mutex mu;
    std::deque<std::vector<uint8_t>> outbox;

    // Set by on_close once the connection is going away; writer_loop exits
    // once it observes this and an empty queue. writer_done is then set by
    // writer_loop so on_close knows it is safe to let the Socket destruct.
    std::atomic<bool> open{true};
    std::atomic<bool> writer_done{false};

    void push(std::vector<uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(mu);
        outbox.push_back(std::move(bytes));
    }

    // Swap out everything queued so far; sending happens outside the lock.
    std::deque<std::vector<uint8_t>> drain() {
        std::deque<std::vector<uint8_t>> out;
        std::lock_guard<std::mutex> lock(mu);
        std::swap(out, outbox);
        return out;
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mu);
        return outbox.empty();
    }
};

// Drains one Session's outbox on an interval, performing the only
// send_message() calls that ever touch that connection's socket.
spaznet::Task writer_loop(std::shared_ptr<Session> session) {
    using namespace std::chrono_literals;
    auto* ctx = session->sock->context();
    while (true) {
        auto pending = session->drain();
        for (auto& msg : pending) {
            co_await spaznet::websocket::send_message(*session->sock,
                                                       spaznet::websocket::Opcode::Text, msg);
        }
        if (!session->open.load(std::memory_order_acquire) && session->empty()) {
            break;
        }
        co_await ctx->interval(10ms);
    }
    session->writer_done.store(true, std::memory_order_release);
}

class ChatRoom : public spaznet::websocket::Handler {
  public:
    spaznet::Task on_open(spaznet::Socket& socket) override {
        auto session = std::make_shared<Session>(socket.fd(), &socket);
        std::size_t online = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            sessions_[session->id] = session;
            online = sessions_.size();
        }
        socket.context()->schedule(writer_loop(session));

        std::string notice = "* user" + std::to_string(session->id) + " joined (" +
                             std::to_string(online) + " online)\n";
        broadcast(session->id, {notice.begin(), notice.end()});
        co_return;
    }

    // Only the const& overload is implemented (it's the pure-virtual one);
    // the rvalue overload's default forwarder calls it, which is fine here
    // since we don't need to move the payload out of `message`.
    spaznet::Task handle_message(const spaznet::websocket::Message& message,
                                 spaznet::Socket& socket) override {
        if (message.opcode != spaznet::websocket::Opcode::Text) {
            co_return;
        }
        std::string framed = "user" + std::to_string(socket.fd()) + ": " +
                             std::string(message.data.begin(), message.data.end());
        broadcast(socket.fd(), {framed.begin(), framed.end()});
        co_return;
    }

    spaznet::Task on_close(spaznet::Socket& socket) override {
        std::shared_ptr<Session> session;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = sessions_.find(socket.fd());
            if (it != sessions_.end()) {
                session = it->second;
                sessions_.erase(it);
            }
        }
        if (!session) {
            co_return;
        }

        std::string notice = "* user" + std::to_string(session->id) + " left\n";
        broadcast(session->id, {notice.begin(), notice.end()});

        // Tell writer_loop to drain whatever is left and stop; then wait
        // for it to finish before returning, since our caller
        // (serve_websocket) destroys `socket` right after on_close.
        session->open.store(false, std::memory_order_release);
        auto* ctx = socket.context();
        while (!session->writer_done.load(std::memory_order_acquire)) {
            co_await ctx->sleep_for(std::chrono::milliseconds(5));
        }
    }

  private:
    // Pushes `bytes` onto every session's outbox except `exclude_id`.
    // A broadcast that races a concurrent on_close for the excluded id is
    // benign: sessions_ is only mutated under mu_, so a snapshot taken
    // before an erase may still deliver one last message to a session
    // whose writer_loop has already exited — the message is dropped along
    // with the Session object, but nothing races the socket itself.
    void broadcast(int exclude_id, std::vector<uint8_t> bytes) {
        std::vector<std::shared_ptr<Session>> targets;
        {
            std::lock_guard<std::mutex> lock(mu_);
            targets.reserve(sessions_.size());
            for (auto& [id, session] : sessions_) {
                if (id != exclude_id) {
                    targets.push_back(session);
                }
            }
        }
        for (auto& session : targets) {
            session->push(bytes);
        }
    }

    std::mutex mu_;
    std::unordered_map<int, std::shared_ptr<Session>> sessions_;
};

} // namespace

int main() {
    spaznet::Server server(4);
    server.set_connection_handler(spaznet::websocket::make_dispatcher(
        std::make_unique<HttpFallback>(), std::make_unique<ChatRoom>()));
    server.listen_tcp(9082);
    server.run();
}
