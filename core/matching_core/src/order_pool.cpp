# include "matching/order_pool.hpp"
#include "matching/types.hpp"


namespace matching {

OrderPool::OrderPool(std::size_t capacity) : pool_(capacity) {
    for (auto& o : pool_) {
        o.next = free_head_;
        free_head_ = &o;
    }
}

Order* OrderPool::acquire() {
    if (!free_head_)    return nullptr;

    Order* result = free_head_;
    free_head_ = result->next;

    result->prev = nullptr;
    result->next = nullptr;

    return result;
}

void OrderPool::release(Order* o) {
    o->next = free_head_;
    free_head_ = o;
}

std::size_t OrderPool::capacity() const noexcept {
    return pool_.size();
}

}