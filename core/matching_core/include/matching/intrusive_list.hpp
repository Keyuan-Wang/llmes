#pragma once

#include "types.hpp"

namespace matching {

class IntrusiveList {
private:
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    std::size_t size_ = 0;

public:
    void push_back(Order& o) {
        o.prev = tail_;
        o.next = nullptr;

        if (tail_)      tail_->next = &o;
        else            head_ = &o;
        tail_ = &o;

        ++size_;
    }

    void erase(Order& o) {
        if (o.prev) o.prev->next = o.next;
        else        head_ = o.next;
        if (o.next) o.next->prev = o.prev;
        else        tail_ = o.prev;
        
        o.prev = o.next = nullptr;
        --size_;
    }

    [[nodiscard]] bool empty() const { return head_ == nullptr; }

    [[nodiscard]] Order& front() const { return *head_; }
    
    [[nodiscard]] std::size_t size() const { return size_; }

    const Order* begin() const { return head_; };
    Order* begin() { return head_; };
};

}