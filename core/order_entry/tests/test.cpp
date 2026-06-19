#include "order_entry/codec.hpp"
#include "order_entry/protocol.hpp"

#include <array>
#include <iostream>

using namespace llmes::order_entry;

int main() {
    std::array<std::byte, 64> buf{};

    MessageHeader h;
    h.sequence_numer = 1;
    h.session_id = 42;

    NewOrder order;
    order.client_order_id = 1001;
    order.side = Side::Buy;
    order.price = 12345;
    order.quantity = 10;

    auto n = encode_new_order(h, order, buf);

    MessageHeader decoded_h;
    auto status = decode_header(buf, decoded_h);

    NewOrder decoded_order;
    status = decode_new_order(buf, decoded_order);

    if (decoded_order.client_order_id == 1001)
        std::cout << "id ok" << std::endl;

    if (decoded_order.side == Side::Buy)
        std::cout << "side ok" << std::endl;

    if (decoded_order.price == 12345)
        std::cout << "price ok" << std::endl;

    if (decoded_order.quantity == 10)
        std::cout << "quantity ok" << std::endl;

}