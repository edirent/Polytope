#pragma once
#include <cstddef>
#include <deque>
#include <stdexcept>
#include <utility>
namespace trading_engine::memory {
// Bounded FIFO storage. Caller provides synchronization (runtime is one consumer,
// venue callback producers serialize under the source queue mutex).
template<class T> class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity):capacity_(capacity) {if(!capacity)throw std::invalid_argument("zero queue capacity");}
    bool push_back(T value) {if(queue_.size()>=capacity_)return false;queue_.push_back(std::move(value));return true;}
    T& front(){return queue_.front();}
    void pop_front(){queue_.pop_front();}
    std::size_t size()const{return queue_.size();}
    bool empty()const{return queue_.empty();}
private:
    std::size_t capacity_;std::deque<T> queue_;
};
}
