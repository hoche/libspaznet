#include <libspaznet/http3/h3frame.hpp>

#include <libspaznet/quic/varint.hpp>

namespace spaznet {
namespace http3 {

namespace {
using spaznet::quic::VarInt;
} // namespace

auto parse_h3_frame(std::span<const uint8_t> buf, std::size_t& offset, H3Frame& out) -> bool {
    const std::size_t start = offset;
    uint64_t type = 0;
    uint64_t length = 0;
    if (!VarInt::decode(buf.data(), buf.size(), offset, type) ||
        !VarInt::decode(buf.data(), buf.size(), offset, length)) {
        offset = start;
        return false;
    }
    if (offset + length > buf.size()) {
        offset = start;
        return false;
    }
    std::span<const uint8_t> body{buf.data() + offset, static_cast<std::size_t>(length)};
    offset += static_cast<std::size_t>(length);

    switch (static_cast<H3FrameType>(type)) {
        case H3FrameType::Data:
            out = H3Data{std::vector<uint8_t>(body.begin(), body.end())};
            return true;
        case H3FrameType::Headers:
            out = H3Headers{std::vector<uint8_t>(body.begin(), body.end())};
            return true;
        case H3FrameType::Settings: {
            H3Settings s;
            std::size_t off = 0;
            while (off < body.size()) {
                uint64_t id = 0;
                uint64_t val = 0;
                if (!VarInt::decode(body.data(), body.size(), off, id) ||
                    !VarInt::decode(body.data(), body.size(), off, val)) {
                    return false;
                }
                s.entries.emplace_back(id, val);
            }
            out = std::move(s);
            return true;
        }
        case H3FrameType::GoAway: {
            std::size_t off = 0;
            uint64_t v = 0;
            if (!VarInt::decode(body.data(), body.size(), off, v) || off != body.size()) {
                return false;
            }
            out = H3GoAway{v};
            return true;
        }
        case H3FrameType::MaxPushId: {
            std::size_t off = 0;
            uint64_t v = 0;
            if (!VarInt::decode(body.data(), body.size(), off, v) || off != body.size()) {
                return false;
            }
            out = H3MaxPushId{v};
            return true;
        }
        case H3FrameType::CancelPush: {
            std::size_t off = 0;
            uint64_t v = 0;
            if (!VarInt::decode(body.data(), body.size(), off, v) || off != body.size()) {
                return false;
            }
            out = H3CancelPush{v};
            return true;
        }
        default:
            // PushPromise is not used server→client; treat like unknown.
            out = H3Reserved{type, std::vector<uint8_t>(body.begin(), body.end())};
            return true;
    }
}

auto encode_h3_frame(std::vector<uint8_t>& out, const H3Frame& f) -> void {
    std::visit(
        [&out](auto const& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, H3Data>) {
                VarInt::append(out, static_cast<uint64_t>(H3FrameType::Data));
                VarInt::append(out, x.data.size());
                out.insert(out.end(), x.data.begin(), x.data.end());
            } else if constexpr (std::is_same_v<T, H3Headers>) {
                VarInt::append(out, static_cast<uint64_t>(H3FrameType::Headers));
                VarInt::append(out, x.encoded_field_section.size());
                out.insert(out.end(), x.encoded_field_section.begin(),
                           x.encoded_field_section.end());
            } else if constexpr (std::is_same_v<T, H3Settings>) {
                std::vector<uint8_t> body;
                for (auto const& [id, v] : x.entries) {
                    VarInt::append(body, id);
                    VarInt::append(body, v);
                }
                VarInt::append(out, static_cast<uint64_t>(H3FrameType::Settings));
                VarInt::append(out, body.size());
                out.insert(out.end(), body.begin(), body.end());
            } else if constexpr (std::is_same_v<T, H3GoAway>) {
                std::vector<uint8_t> body;
                VarInt::append(body, x.stream_or_push_id);
                VarInt::append(out, static_cast<uint64_t>(H3FrameType::GoAway));
                VarInt::append(out, body.size());
                out.insert(out.end(), body.begin(), body.end());
            } else if constexpr (std::is_same_v<T, H3MaxPushId>) {
                std::vector<uint8_t> body;
                VarInt::append(body, x.value);
                VarInt::append(out, static_cast<uint64_t>(H3FrameType::MaxPushId));
                VarInt::append(out, body.size());
                out.insert(out.end(), body.begin(), body.end());
            } else if constexpr (std::is_same_v<T, H3CancelPush>) {
                std::vector<uint8_t> body;
                VarInt::append(body, x.push_id);
                VarInt::append(out, static_cast<uint64_t>(H3FrameType::CancelPush));
                VarInt::append(out, body.size());
                out.insert(out.end(), body.begin(), body.end());
            } else if constexpr (std::is_same_v<T, H3Reserved>) {
                VarInt::append(out, x.type);
                VarInt::append(out, x.data.size());
                out.insert(out.end(), x.data.begin(), x.data.end());
            }
        },
        f);
}

} // namespace http3
} // namespace spaznet
