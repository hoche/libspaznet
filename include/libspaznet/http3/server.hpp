#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <libspaznet/http3/qpack.hpp>
#include <libspaznet/quic/connection.hpp>

namespace spaznet {
namespace http3 {

// Application-level request and response surfaced to user code.
struct Http3Request {
    std::string method;
    std::string scheme;
    std::string authority;
    std::string path;
    HeaderList headers; // non-pseudo headers
    std::vector<uint8_t> body;
};

struct Http3Response {
    int status_code{200};
    HeaderList headers;
    std::vector<uint8_t> body;
};

// Server-side HTTP/3 layer that sits on top of a single quic::Connection.
// One Http3Server per Connection.
//
// Lifecycle:
//   - Construct, passing the Connection (must outlive this object) and a
//     request-dispatch callback.
//   - Each time the Connection processes a datagram (or after the
//     handshake completes), call `pump()` to feed any newly-available
//     stream bytes into the HTTP/3 state machine.
class Http3Server {
  public:
    using RequestFn = std::function<Http3Response(const Http3Request&)>;

    Http3Server(quic::Connection& conn, RequestFn on_request);

    Http3Server(const Http3Server&) = delete;
    auto operator=(const Http3Server&) -> Http3Server& = delete;
    Http3Server(Http3Server&&) = delete;
    auto operator=(Http3Server&&) -> Http3Server& = delete;
    ~Http3Server() = default;

    // Drive the HTTP/3 machinery forward. Safe to call repeatedly;
    // idempotent if no new bytes are available.
    auto pump() -> void;

    // Whether we've already initialized the server-side uni streams
    // (control + QPACK encoder + QPACK decoder).
    [[nodiscard]] auto initialized() const -> bool {
        return initialized_;
    }

  private:
    auto initialize_server_streams() -> void;
    auto process_stream(uint64_t id, quic::Stream& s) -> void;
    auto handle_request_data(uint64_t id) -> void;

    quic::Connection& conn_;
    RequestFn on_request_;
    bool initialized_{false};

    // Server-initiated unidirectional stream IDs: 4*n + 3. We use 3 for
    // control, 7 for QPACK encoder, 11 for QPACK decoder (RFC 9114 §6.2,
    // RFC 9204 §4.2).
    static constexpr uint64_t kServerControl = 3;
    static constexpr uint64_t kServerQpackEncoder = 7;
    static constexpr uint64_t kServerQpackDecoder = 11;

    // Per request bidi stream: incoming bytes buffered until we have
    // enough to parse the HEADERS frame and any DATA.
    struct PendingRequest {
        std::vector<uint8_t> recv_buf;
        Http3Request req;
        bool headers_parsed{false};
        bool fin_seen{false};
        bool handled{false};
    };
    std::map<uint64_t, PendingRequest> pending_;

    // Per incoming uni stream: type identifier (first varint) and
    // buffered bytes for control-stream H3 frame parsing.
    struct PendingUni {
        uint64_t type{UINT64_MAX};
        std::vector<uint8_t> buf;
    };
    std::map<uint64_t, PendingUni> uni_;
};

} // namespace http3
} // namespace spaznet
