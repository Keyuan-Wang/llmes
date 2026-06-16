#include <thread>
#include <iostream>

#include "spsc_ring_buffer.hpp"

int main() {
    SpscRingBuffer<int, 8> queue;

    constexpr int total = 100'000;

    std::thread producer([&] {
        for (int value = 1; value <= total; ++value) {
            while (!queue.push(value)) {
                // queue full, retry
            }
        }
    });

    std::thread consumer([&] {
        for (int expected = 1; expected <= total; ++expected) {
            std::optional<int> value;

            while (!(value = queue.pop())) {
                // queue empty, retry
            }

            if (*value != expected) {
                std::cout << "error: expected "
                          << expected
                          << ", got "
                          << *value
                          << '\n';
                return;
            }
        }

        std::cout << "ok\n";
    });

    producer.join();
    consumer.join();
}