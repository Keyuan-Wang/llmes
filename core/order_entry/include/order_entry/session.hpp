#pragma once

#include "order_entry/codec.hpp"
#include "order_entry/frame_parser.hpp"
#include "order_entry/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace llmes::order_entry {

class OrderEntrySession {
public:
    explicit OrderEntrySession(std::uint64_t session_id = 0) noexcept
        : session_id_(session_id) {}

    bool handle_message(const DecodedMessage& msg, std::span<std::byte> response_out);

    [[nodiscard]] std::uint64_t expected_sequence_number() const noexcept {
        return expected_sequence_number_;
    }

    [[nodiscard]] std::uint64_t active_order_count() const noexcept {
        return static_cast<std::uint64_t>(orders_.size());
    }

private:
    std::uint64_t session_id_ = 0;
    std::uint64_t expected_sequence_number_ = 1;
    std::uint64_t next_order_handle_ = 1;
    std::unordered_map<std::uint64_t, std::uint64_t> orders_;

    bool check_sequence_or_reject(const DecodedMessage& msg,
                                  std::uint64_t client_order_id,
                                  std::span<std::byte> response_out);

    void encode_reject(const MessageHeader& request_header,
                       std::uint64_t client_order_id,
                       RejectReason reason,
                       std::span<std::byte> response_out) const;

    void encode_accept(const MessageHeader& request_header,
                       std::uint64_t client_order_id,
                       std::uint64_t order_handle,
                       std::span<std::byte> response_out) const;

    void encode_cancel(const MessageHeader& request_header,
                       std::uint64_t client_order_id,
                       std::uint64_t order_handle,
                       std::span<std::byte> response_out) const;

    void encode_modify(const MessageHeader& request_header,
                       std::uint64_t client_order_id,
                       std::uint64_t order_handle,
                       std::span<std::byte> response_out) const;
};

inline bool OrderEntrySession::handle_message(const DecodedMessage& msg,
                                              std::span<std::byte> response_out) {
    switch (msg.type) {
        case MessageType::NewOrder: {
            const auto client_order_id = msg.new_order.client_order_id;
            if (!check_sequence_or_reject(msg, client_order_id, response_out))
                return true;

            if (msg.new_order.price == 0) {
                encode_reject(msg.header, client_order_id, RejectReason::InvalidPrice, response_out);
                return true;
            }

            if (msg.new_order.quantity == 0) {
                encode_reject(msg.header, client_order_id, RejectReason::InvalidQuantity, response_out);
                return true;
            }

            if (orders_.find(client_order_id) != orders_.end()) {
                encode_reject(msg.header, client_order_id, RejectReason::DuplicateClientOrderId, response_out);
                return true;
            }

            const std::uint64_t order_handle = next_order_handle_++;
            orders_.emplace(client_order_id, order_handle);
            encode_accept(msg.header, client_order_id, order_handle, response_out);
            return true;
        }

        case MessageType::CancelOrder: {
            const auto client_order_id = msg.cancel_order.client_order_id;
            if (!check_sequence_or_reject(msg, client_order_id, response_out))
                return true;

            const auto it = orders_.find(client_order_id);
            if (it == orders_.end()) {
                encode_reject(msg.header, client_order_id, RejectReason::UnknownClientOrderId, response_out);
                return true;
            }

            const std::uint64_t order_handle = it->second;
            orders_.erase(it);
            encode_cancel(msg.header, client_order_id, order_handle, response_out);
            return true;
        }

        case MessageType::ModifyOrder: {
            const auto client_order_id = msg.modify_order.client_order_id;
            if (!check_sequence_or_reject(msg, client_order_id, response_out))
                return true;

            const auto it = orders_.find(client_order_id);
            if (it == orders_.end()) {
                encode_reject(msg.header, client_order_id, RejectReason::UnknownClientOrderId, response_out);
                return true;
            }

            if (msg.modify_order.new_price == 0) {
                encode_reject(msg.header, client_order_id, RejectReason::InvalidPrice, response_out);
                return true;
            }

            if (msg.modify_order.new_quantity == 0) {
                encode_reject(msg.header, client_order_id, RejectReason::InvalidQuantity, response_out);
                return true;
            }

            encode_modify(msg.header, client_order_id, it->second, response_out);
            return true;
        }

        case MessageType::Heartbeat:
            if (!check_sequence_or_reject(msg, 0, response_out))
                return true;
            encode_accept(msg.header, 0, 0, response_out);
            return true;

        case MessageType::Logout:
            if (!check_sequence_or_reject(msg, 0, response_out))
                return true;
            encode_accept(msg.header, 0, 0, response_out);
            return false;

        default:
            encode_reject(msg.header, 0, RejectReason::UnsupportedMessageType, response_out);
            return true;
    }
}

inline bool OrderEntrySession::check_sequence_or_reject(const DecodedMessage& msg,
                                                        std::uint64_t client_order_id,
                                                        std::span<std::byte> response_out) {
    if (msg.header.sequence_number != expected_sequence_number_) {
        encode_reject(msg.header, client_order_id, RejectReason::BadSequence, response_out);
        return false;
    }

    ++expected_sequence_number_;
    return true;
}

inline void OrderEntrySession::encode_reject(const MessageHeader& request_header,
                                             std::uint64_t client_order_id,
                                             RejectReason reason,
                                             std::span<std::byte> response_out) const {
    MessageHeader response_header = request_header;
    response_header.session_id = session_id_ == 0 ? request_header.session_id : session_id_;

    Rejected response;
    response.client_order_id = client_order_id;
    response.reason = reason;

    encode_rejected(response_header, response, response_out);
}

inline void OrderEntrySession::encode_accept(const MessageHeader& request_header,
                                             std::uint64_t client_order_id,
                                             std::uint64_t order_handle,
                                             std::span<std::byte> response_out) const {
    MessageHeader response_header = request_header;
    response_header.session_id = session_id_ == 0 ? request_header.session_id : session_id_;

    Accepted response;
    response.client_order_id = client_order_id;
    response.order_handle = order_handle;

    encode_accepted(response_header, response, response_out);
}

inline void OrderEntrySession::encode_cancel(const MessageHeader& request_header,
                                             std::uint64_t client_order_id,
                                             std::uint64_t order_handle,
                                             std::span<std::byte> response_out) const {
    MessageHeader response_header = request_header;
    response_header.session_id = session_id_ == 0 ? request_header.session_id : session_id_;

    Cancelled response;
    response.client_order_id = client_order_id;
    response.order_handle = order_handle;

    encode_cancelled(response_header, response, response_out);
}

inline void OrderEntrySession::encode_modify(const MessageHeader& request_header,
                                             std::uint64_t client_order_id,
                                             std::uint64_t order_handle,
                                             std::span<std::byte> response_out) const {
    MessageHeader response_header = request_header;
    response_header.session_id = session_id_ == 0 ? request_header.session_id : session_id_;

    Modified response;
    response.client_order_id = client_order_id;
    response.order_handle = order_handle;

    encode_modified(response_header, response, response_out);
}

} // namespace llmes::order_entry
