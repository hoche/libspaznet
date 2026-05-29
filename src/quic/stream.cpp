#include <libspaznet/quic/stream.hpp>

#include <algorithm>
#include <cstring>

namespace spaznet {
namespace quic {

auto stream_type(uint64_t stream_id) -> StreamType {
    return static_cast<StreamType>(stream_id & 0x03U);
}

auto is_bidi(uint64_t stream_id) -> bool {
    return (stream_id & 0x02U) == 0;
}

auto is_client_initiated(uint64_t stream_id) -> bool {
    return (stream_id & 0x01U) == 0;
}

Stream::Stream(uint64_t stream_id, uint64_t initial_max_recv, uint64_t initial_max_send)
    : recv_limit_(initial_max_recv), send_limit_(initial_max_send), id_(stream_id) {}

// ---- Receive side ---------------------------------------------------------

auto Stream::deliver(uint64_t offset, const std::vector<uint8_t>& data, bool fin) -> bool {
    if (recv_state_ == RecvState::ResetRecvd || recv_state_ == RecvState::ResetRead) {
        return true; // ignore data after reset (peer race is allowed)
    }
    const uint64_t end = offset + data.size();
    if (end > recv_limit_) {
        return false; // FLOW_CONTROL_ERROR
    }
    if (recv_final_size_ && (end > *recv_final_size_ ||
                             (fin && end != *recv_final_size_))) {
        return false; // FINAL_SIZE_ERROR
    }
    if (fin) {
        recv_final_size_ = end;
    }
    if (end > recv_high_water_) {
        recv_high_water_ = end;
    }

    // Trim against the already-consumed prefix.
    uint64_t put_off = offset;
    std::vector<uint8_t> put_data = data;
    if (put_off < recv_read_offset_) {
        const uint64_t drop = recv_read_offset_ - put_off;
        if (drop >= put_data.size()) {
            // entirely before our cursor — but still update FIN state below
            put_data.clear();
            put_off = recv_read_offset_;
        } else {
            put_data.erase(put_data.begin(),
                           put_data.begin() + static_cast<std::ptrdiff_t>(drop));
            put_off = recv_read_offset_;
        }
    }
    if (!put_data.empty()) {
        // Merge with adjacent existing chunks.
        auto it = recv_chunks_.lower_bound(put_off);
        // Check chunk immediately before.
        if (it != recv_chunks_.begin()) {
            auto prev = std::prev(it);
            const uint64_t prev_end = prev->first + prev->second.size();
            if (prev_end >= put_off) {
                if (prev_end >= put_off + put_data.size()) {
                    // already covered
                    put_data.clear();
                } else {
                    const uint64_t overlap = prev_end - put_off;
                    put_data.erase(put_data.begin(),
                                   put_data.begin() + static_cast<std::ptrdiff_t>(overlap));
                    put_off = prev_end;
                    // Append remainder to prev chunk.
                    prev->second.insert(prev->second.end(), put_data.begin(), put_data.end());
                    put_off = prev->first;
                    put_data.clear();
                    // Continue merging forward from prev.
                    it = std::next(prev);
                    while (it != recv_chunks_.end() &&
                           it->first <= prev->first + prev->second.size()) {
                        const uint64_t cur_end = it->first + it->second.size();
                        if (cur_end > prev->first + prev->second.size()) {
                            const std::size_t take = static_cast<std::size_t>(
                                cur_end - (prev->first + prev->second.size()));
                            prev->second.insert(prev->second.end(),
                                                it->second.end() -
                                                    static_cast<std::ptrdiff_t>(take),
                                                it->second.end());
                        }
                        it = recv_chunks_.erase(it);
                    }
                }
            }
        }
        if (!put_data.empty()) {
            // Place new chunk at put_off. Then merge forward.
            auto [ins, ok] = recv_chunks_.emplace(put_off, std::move(put_data));
            (void)ok;
            auto nxt = std::next(ins);
            while (nxt != recv_chunks_.end() &&
                   nxt->first <= ins->first + ins->second.size()) {
                const uint64_t cur_end = nxt->first + nxt->second.size();
                if (cur_end > ins->first + ins->second.size()) {
                    const std::size_t take = static_cast<std::size_t>(
                        cur_end - (ins->first + ins->second.size()));
                    ins->second.insert(ins->second.end(),
                                       nxt->second.end() -
                                           static_cast<std::ptrdiff_t>(take),
                                       nxt->second.end());
                }
                nxt = recv_chunks_.erase(nxt);
            }
        }
    }

    // State transitions per RFC 9000 §3.2.
    if (recv_final_size_) {
        if (recv_state_ == RecvState::Recv) {
            recv_state_ = RecvState::SizeKnown;
        }
        // Did we receive everything below final_size contiguously?
        if (!recv_chunks_.empty()) {
            auto& first = *recv_chunks_.begin();
            if (first.first == recv_read_offset_ &&
                first.first + first.second.size() == *recv_final_size_ &&
                recv_chunks_.size() == 1) {
                recv_state_ = RecvState::DataRecvd;
            }
        } else if (recv_read_offset_ == *recv_final_size_) {
            // Pure FIN with no data; all bytes already read.
            recv_state_ = RecvState::DataRead;
        }
    }
    return true;
}

auto Stream::read_contiguous(std::vector<uint8_t>& out, bool& fin_out) -> std::size_t {
    fin_out = false;
    if (recv_chunks_.empty()) {
        if (recv_final_size_ && recv_read_offset_ == *recv_final_size_) {
            fin_out = true;
            if (recv_state_ != RecvState::DataRead) {
                recv_state_ = RecvState::DataRead;
            }
        }
        return 0;
    }
    auto& first = *recv_chunks_.begin();
    if (first.first != recv_read_offset_) {
        return 0; // hole
    }
    const std::size_t before = out.size();
    out.insert(out.end(), first.second.begin(), first.second.end());
    const std::size_t copied = out.size() - before;
    recv_read_offset_ += copied;
    recv_chunks_.erase(recv_chunks_.begin());

    if (recv_final_size_ && recv_read_offset_ == *recv_final_size_) {
        fin_out = true;
        recv_state_ = RecvState::DataRead;
    } else if (recv_state_ == RecvState::DataRecvd) {
        // had everything but hadn't read it yet — still in DataRecvd
    }
    return copied;
}

auto Stream::set_recv_limit(uint64_t new_limit) -> bool {
    if (new_limit <= recv_limit_) {
        return false;
    }
    recv_limit_ = new_limit;
    return true;
}

auto Stream::reset_recvd(uint64_t final_size, uint64_t app_error) -> bool {
    if (recv_final_size_ && *recv_final_size_ != final_size) {
        return false; // FINAL_SIZE_ERROR
    }
    if (final_size > recv_limit_) {
        return false;
    }
    recv_final_size_ = final_size;
    recv_high_water_ = std::max(recv_high_water_, final_size);
    reset_error_ = app_error;
    recv_state_ = RecvState::ResetRecvd;
    recv_chunks_.clear();
    return true;
}

// ---- Send side ------------------------------------------------------------

auto Stream::write(const std::vector<uint8_t>& bytes, bool fin) -> void {
    if (send_state_ == SendState::ResetSent || send_state_ == SendState::ResetRecvd ||
        send_state_ == SendState::DataSent || send_state_ == SendState::DataRecvd) {
        return;
    }
    send_buf_.insert(send_buf_.end(), bytes.begin(), bytes.end());
    send_high_water_ += bytes.size();
    if (fin) {
        send_fin_queued_ = true;
    }
    if (send_state_ == SendState::Ready) {
        send_state_ = SendState::Send;
    }
}

auto Stream::pull_send(std::size_t max_bytes, uint64_t& offset_out,
                       std::vector<uint8_t>& data_out, bool& fin_out) -> std::size_t {
    data_out.clear();
    fin_out = false;
    if (max_bytes == 0) {
        return 0;
    }

    // Retransmissions take priority.
    if (!lost_ranges_.empty()) {
        auto& r = lost_ranges_.front();
        const std::size_t take = std::min(max_bytes, r.second);
        offset_out = r.first;
        const std::size_t buf_off =
            static_cast<std::size_t>(r.first - send_buf_base_offset_);
        data_out.assign(send_buf_.begin() + static_cast<std::ptrdiff_t>(buf_off),
                        send_buf_.begin() + static_cast<std::ptrdiff_t>(buf_off + take));
        // FIN may be coincident with retransmitted final byte.
        if (send_fin_queued_ && r.first + take == send_high_water_) {
            fin_out = true;
            send_fin_sent_ = true;
        }
        if (take == r.second) {
            lost_ranges_.erase(lost_ranges_.begin());
        } else {
            r.first += take;
            r.second -= take;
        }
        return take;
    }

    // Fresh send. Bounded by flow-control limit.
    if (send_next_offset_ >= send_limit_) {
        // We may still be able to emit a FIN-only chunk if everything
        // up to send_high_water_ was already on the wire and we haven't
        // FIN'd yet.
        if (send_fin_queued_ && !send_fin_sent_ &&
            send_next_offset_ == send_high_water_) {
            offset_out = send_next_offset_;
            fin_out = true;
            send_fin_sent_ = true;
            if (send_state_ == SendState::Send) {
                send_state_ = SendState::DataSent;
            }
        }
        return 0;
    }
    const uint64_t window = send_limit_ - send_next_offset_;
    const uint64_t avail = send_high_water_ - send_next_offset_;
    const std::size_t take =
        static_cast<std::size_t>(std::min<uint64_t>({max_bytes, window, avail}));
    if (take == 0) {
        if (send_fin_queued_ && !send_fin_sent_ &&
            send_next_offset_ == send_high_water_) {
            offset_out = send_next_offset_;
            fin_out = true;
            send_fin_sent_ = true;
            if (send_state_ == SendState::Send) {
                send_state_ = SendState::DataSent;
            }
        }
        return 0;
    }
    offset_out = send_next_offset_;
    const std::size_t buf_off =
        static_cast<std::size_t>(send_next_offset_ - send_buf_base_offset_);
    data_out.assign(send_buf_.begin() + static_cast<std::ptrdiff_t>(buf_off),
                    send_buf_.begin() + static_cast<std::ptrdiff_t>(buf_off + take));
    send_next_offset_ += take;
    if (send_fin_queued_ && send_next_offset_ == send_high_water_ && !send_fin_sent_) {
        fin_out = true;
        send_fin_sent_ = true;
        if (send_state_ == SendState::Send) {
            send_state_ = SendState::DataSent;
        }
    }
    return take;
}

auto Stream::on_acked(uint64_t offset, std::size_t length) -> void {
    if (length == 0) {
        // Pure-FIN ack.
        if (send_fin_sent_) {
            send_fin_acked_ = true;
            try_advance_send_state_after_ack();
        }
        return;
    }
    acked_ranges_.emplace_back(offset, length);
    // Coalesce / trim from the front of send_buf_.
    std::sort(acked_ranges_.begin(), acked_ranges_.end());
    while (!acked_ranges_.empty() && acked_ranges_.front().first == send_buf_base_offset_) {
        const std::size_t take = acked_ranges_.front().second;
        send_buf_.erase(send_buf_.begin(),
                        send_buf_.begin() + static_cast<std::ptrdiff_t>(take));
        send_buf_base_offset_ += take;
        acked_ranges_.erase(acked_ranges_.begin());
    }
    // The ack might also include the FIN byte position.
    if (send_fin_sent_ && offset + length >= send_high_water_) {
        send_fin_acked_ = true;
    }
    try_advance_send_state_after_ack();
}

auto Stream::on_lost(uint64_t offset, std::size_t length) -> void {
    if (length > 0) {
        lost_ranges_.emplace_back(offset, length);
    }
    if (send_fin_sent_ && offset + length >= send_high_water_) {
        send_fin_sent_ = false; // need to re-send FIN with retransmission
    }
}

auto Stream::set_send_limit(uint64_t new_limit) -> void {
    if (new_limit > send_limit_) {
        send_limit_ = new_limit;
    }
}

auto Stream::try_advance_send_state_after_ack() -> void {
    if (send_state_ == SendState::DataSent && send_buf_.empty() && send_fin_acked_) {
        send_state_ = SendState::DataRecvd;
    }
}

} // namespace quic
} // namespace spaznet
