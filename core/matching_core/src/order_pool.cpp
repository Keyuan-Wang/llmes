# include "matching/order_pool.hpp"
#include "matching/types.hpp"
#include <cassert>


namespace matching {

OrderPool::OrderPool(std::size_t capacity) : pool_(capacity) {
    for (auto& o : pool_) {
        o.next = free_head_;
        free_head_ = &o;
    }
}

void OrderPool::release(Order* o) {
    o->next = free_head_;
    free_head_ = o;
}


Order* OrderPool::resolve(OrderHandle h) noexcept {
    assert(h != kInvalidHandle);
    assert(static_cast<std::size_t>(h) < pool_.size());
    return &pool_[h];
}

std::size_t OrderPool::capacity() const noexcept {
    return pool_.size();
}

}