#pragma once

#include "order_entry/codec.hpp"
#include "order_entry/protocol.hpp"

#include <cstring>
#include <array>
#include <span>
#include <bit>

namespace llmes::order_entry {


struct DecodedMessage {
    MessageHeader header{};
    MessageType type{};

    NewOrder new_order{};
    CancelOrder cancel_order{};
    ModifyOrder modify_order{};
};



template <std::size_t Capacity>
class FrameParser {
public:
    static_assert(Capacity % kFrameSize == 0);
    // check if power of 2
    static_assert(std::has_single_bit(Capacity));

    enum class Status {
        MessageReady,
        NeedMoreData,
        ProtocolError,
        BufferFull
    };

    // append bytes read by socket to internal buffer
    bool append(std::span<const std::byte> bytes);

    Status try_parse(DecodedMessage& out);

    // number of occupied slots
    [[gnu::always_inline]] inline std::size_t size() const noexcept { return write_pos_ - read_pos_; }
    // number of free slots left
    [[gnu::always_inline]] inline std::size_t free_slots() const noexcept { return Capacity - size(); }
    // check if full
    [[gnu::always_inline]] inline bool full() const noexcept { return size() == Capacity; } 



private:
    std::array<std::byte, Capacity> buffer_{};
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;

    static constexpr std::size_t buffer_idx(std::size_t index) {
        return index & (Capacity - 1);
    }

    // fixed-size frames keep read_pos_ frame-aligned, so returned 64B span is physically contiguous
    [[nodiscard]] std::span<const std::byte> readable_span() const noexcept {
        return {buffer_.data() + buffer_idx(read_pos_), kFrameSize};
    };
    
    [[gnu::always_inline]] inline void move_write_pos(std::size_t step) noexcept { write_pos_ += step; }
    [[gnu::always_inline]] inline void move_read_pos(std::size_t step) noexcept { read_pos_ += step; }
};





template <std::size_t Capacity>
bool FrameParser<Capacity>::append(std::span<const std::byte> bytes) {
    if (bytes.empty())      return true;

    // not enough space
    if (free_slots() < bytes.size())   return false;


    // -------------------------------- Move bytes to buffer --------------------------------

    // 1. bytes can be stored in contiguous memory
    if (buffer_idx(write_pos_) + bytes.size() <= Capacity) [[likely]]
        std::memcpy(buffer_.data() + buffer_idx(write_pos_), bytes.data(), bytes.size());
    // 2. bytes needs to be separated
    else {
        std::size_t bytes_head = Capacity - buffer_idx(write_pos_);
        std::size_t bytes_tail = bytes.size() - bytes_head;

        std::memcpy(buffer_.data() + buffer_idx(write_pos_), bytes.data(), bytes_head);
        std::memcpy(buffer_.data(), bytes.data() + bytes_head, bytes_tail);
    }

    // Only update the write pos when data is actually stored in buffer
    move_write_pos(bytes.size());
    return true;
}


template <std::size_t Capacity>
inline FrameParser<Capacity>::Status FrameParser<Capacity>::try_parse(DecodedMessage& out) {
    // number of wrote-in slots
    const std::size_t available = size();

    // ------------------------------------- Parse header -------------------------------------

    // if wrote-in data shorter than header size, then must be incomplete
    if (available < kFrameSize)
        return Status::NeedMoreData;

    const auto span = readable_span();

    // decode header
    MessageHeader header{};
    DecodeStatus status = decode_header(span.first(kHeaderSize), header);

    if (status != DecodeStatus::Ok)
        return Status::ProtocolError;

    // validate header
    status = validate_header(header);

    if (status != DecodeStatus::Ok)
        return Status::ProtocolError;



    // ------------------------------------- Parse order -------------------------------------

    out.header = header;
    out.type = header.message_type;

    switch(header.message_type) {
        case MessageType::NewOrder:
            status = decode_new_order(span, out.new_order);
            break;

        case MessageType::CancelOrder:
            status = decode_cancel_order(span, out.cancel_order);
            break;
    
        case MessageType::ModifyOrder:
            status = decode_modify_order(span, out.modify_order);
            break;

        case MessageType::Heartbeat:

        case MessageType::Logout:
            status = DecodeStatus::Ok;
            break;

        default:
            return Status::ProtocolError;
    }

    if (status != DecodeStatus::Ok)
        return Status::ProtocolError;

    // Only move read pos when the parsing process is finished
    move_read_pos(kFrameSize);

    return Status::MessageReady;
}

}   // namespace llmes::order_entry