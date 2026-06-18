#pragma once

#include <cstdint>


namespace llme::order_entry {

inline constexpr std::uint32_t kMagic = 0x6c6c6d65; // "llme"

inline constexpr std::uint16_t kVersion = 1;

inline constexpr std::size_t kHeaderSize = 32;

// compact message
enum class MessageType : std::uint16_t {
    NewOrder        = 1,
    CancelOrder     = 2,
    ModifyOrder     = 3,
    Heartbeat       = 4,
    Logout          = 5,

    Accepted        = 101,
    Rejected        = 102,
    Cancelled       = 103,
    Modified        = 104,
    Trade           = 105
};


enum class Side : std::uint64_t {
    Buy  = 1,
    Sell = 2,
};


enum class DecodeStatus {
    Ok,
    NeedMoreData,
    BadMagic,
    BadVersion,
    UnknownMessageType,
    BadPayloadLength
};


// 32-bytes
struct MessageHeader {
    std::uint32_t magic             = kMagic;
    std::uint16_t version           = kVersion;
    MessageType message_type{};
    std::uint16_t payload_length    = 0;
    std::uint16_t flags             = 0;
    std::uint64_t sequence_numer    = 0;
    std::uint64_t session_id        = 0;
    std::uint32_t reserved          = 0;
};


// 32-bytes
struct NewOrder {
    std::uint64_t client_order_id   = 0;
    Side side                       = Side::Buy;
    std::uint64_t price             = 0;
    std::uint64_t quantity          = 0;
};


// 32-bytes
struct CancelOrder {
    std::uint64_t client_order_id   = 0;
    std::uint64_t reserved1;
    std::uint64_t reserved2;
    std::uint64_t reserved3;
};


// 32-bytes
struct ModifyOrder {
    std::uint64_t client_order_id   = 0;
    std::uint64_t new_price         = 0;
    std::uint64_t new_quantity      = 0;
    std::uint64_t reserved;
};


struct Hearbeat {};


struct Logout {};


inline constexpr std::uint16_t kNewOrderPayloadSize = 32;
inline constexpr std::uint16_t kCancelOrderPayloadSize = 32;
inline constexpr std::uint16_t kModifyOrderPayloadSize = 32;
inline constexpr std::uint16_t kHeartbeatPayloadSize = 0;
inline constexpr std::uint16_t kLogoutPayloadSize = 0;

}   // namespace llmes::order_entry