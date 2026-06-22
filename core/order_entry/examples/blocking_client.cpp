#include "order_entry/codec.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>


using namespace llmes::order_entry;


namespace {

using Frame = std::array<std::byte, kFrameSize>;

void set_tcp_nodelay(int fd) {
    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}


bool send_all(int fd, const std::byte* data, std::size_t size) {
    std::size_t sent = 0;

    while (sent < size) {
        const std::size_t chunk = std::min<std::size_t>(7, size - sent);

        const ssize_t n = ::send(fd, data + sent, chunk, 0);
        if (n <= 0) {
            perror("send");
            return false;
        }

        sent += static_cast<std::size_t>(n);
    }

    return true;
}

bool recv_all(int fd, std::byte* data, std::size_t size) {
    std::size_t received = 0;

    while (received < size) {
        const ssize_t n = ::recv(fd, data + received, size - received, 0);
        if (n <= 0) {
            perror("recv");
            return false;
        }

        received += static_cast<std::size_t>(n);
    }

    return true;
}

void print_response(const Frame& frame) {
    MessageHeader header;
    if (decode_header(frame, header) != DecodeStatus::Ok ||
        validate_response_header(header) != DecodeStatus::Ok) {
        std::cout << "bad response\n";
        return;
    }

    std::cout << "response seq=" << header.sequence_number
              << " type=" << static_cast<std::uint16_t>(header.message_type);

    switch (header.message_type) {
        case MessageType::Accepted: {
            Accepted accepted;
            decode_accepted(frame, accepted);
            std::cout << " Accepted"
                      << " id=" << accepted.client_order_id
                      << " handle=" << accepted.order_handle;
            break;
        }
        case MessageType::Rejected: {
            Rejected rejected;
            decode_rejected(frame, rejected);
            std::cout << " Rejected"
                      << " id=" << rejected.client_order_id
                      << " reason=" << static_cast<std::uint64_t>(rejected.reason);
            break;
        }
        case MessageType::Cancelled: {
            Cancelled cancelled;
            decode_cancelled(frame, cancelled);
            std::cout << " Cancelled"
                      << " id=" << cancelled.client_order_id
                      << " handle=" << cancelled.order_handle;
            break;
        }
        default:
            std::cout << " Other";
            break;
    }

    std::cout << '\n';
}


Frame make_new_order(std::uint64_t seq) {
    Frame frame{};

    MessageHeader header;
    header.sequence_number = seq;
    header.session_id = 42;

    NewOrder order;
    order.client_order_id = 1001;
    order.side = Side::Buy;
    order.price = 12345;
    order.quantity = 10;

    const auto n = encode_new_order(header, order, frame);
    if (n != kFrameSize) {
        std::abort();
    }

    return frame;
}

Frame make_cancel_order(std::uint64_t seq) {
    Frame frame{};

    MessageHeader header;
    header.sequence_number = seq;
    header.session_id = 42;

    CancelOrder order;
    order.client_order_id = 1001;

    const auto n = encode_cancel_order(header, order, frame);
    if (n != kFrameSize) {
        std::abort();
    }

    return frame;
}

Frame make_logout(std::uint64_t seq) {
    Frame frame{};

    MessageHeader header;
    header.message_type = MessageType::Logout;
    header.payload_length = kPayloadSize;
    header.sequence_number = seq;
    header.session_id = 42;

    encode_header(header, frame);
    return frame;
}

}   // namespace


int main() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    set_tcp_nodelay(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);

    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        std::cerr << "bad address\n";
        ::close(fd);
        return 1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        ::close(fd);
        return 1;
    }

    const auto new_order = make_new_order(1);
    const auto cancel_order = make_cancel_order(2);
    const auto logout = make_logout(3);
    Frame response{};

    if (!send_all(fd, new_order.data(), new_order.size())) {
        ::close(fd);
        return 1;
    }
    if (!recv_all(fd, response.data(), response.size())) {
        ::close(fd);
        return 1;
    }
    print_response(response);

    if (!send_all(fd, cancel_order.data(), cancel_order.size())) {
        ::close(fd);
        return 1;
    }
    if (!recv_all(fd, response.data(), response.size())) {
        ::close(fd);
        return 1;
    }
    print_response(response);

    if (!send_all(fd, logout.data(), logout.size())) {
        ::close(fd);
        return 1;
    }
    if (!recv_all(fd, response.data(), response.size())) {
        ::close(fd);
        return 1;
    }
    print_response(response);

    ::close(fd);
    return 0;
}
