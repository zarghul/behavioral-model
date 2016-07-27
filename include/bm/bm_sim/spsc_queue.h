
/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Yuri Iozzelli (y/iozzelli@gmail.com)
 *
 */

//! @file queue.h

#ifndef BM_BM_SIM_SPSC_QUEUE_H_
#define BM_BM_SIM_SPSC_QUEUE_H_

#include <string>
#include <iostream>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <queue>

#include <cmath>
#include <cassert>

namespace {
class Semaphore {
 public:
  Semaphore():flag(false){}
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this](){ return flag; });
    flag = false;
  }
  void signal() {
    std::unique_lock<std::mutex> lock(mutex);
    flag = true;
    condition.notify_one();
  }
 private:
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  bool flag;
};

constexpr int cons_sleep_time{1}; // microseconds
}


namespace bm{

template <class T>
class SPSCQueue {
 public:
  using index_t = uint64_t;
  using atomic_index_t = std::atomic<index_t>;
  static constexpr size_t max_size = 1ul<<(sizeof(index_t)*8-1);

  SPSCQueue(size_t max_capacity = 1024)
    : ring_capacity(1ul<<uint64_t(ceil(log2(max_capacity)))),
      queue_capacity(max_capacity),
      ring(new T[ring_capacity]) {
    assert(ring_capacity <= max_size);
  }

  //! Moves \p item to the front of the queue (producer)
  bool push_front(T&& item, bool force = true) {
    return push_front_forward(std::move(item), force);
  }
  //! Copies \p item to the front of the queue (producer)
  bool push_front(const T& item, bool force = true) {
    return push_front_forward(item, force);
  }
  //! Pops an element from the back of the queue: moves the element to `*pItem`.
  // (consumer)
  bool pop_back(T* pItem) {
    cons_wait_data(1);
    *pItem = std::move(ring[normalize_index(cons_ci)]);
    cons_advance(1);

    return true;
  }
  //! Pops an element from the back of the queue: moves the element to `*pItem`.
  // (consumer)
  bool pop_back(std::vector<T>* container) {
    index_t num = cons_wait_data(1);
    for (index_t i = 0; i < num; i++) {
      container->push_back(std::move(ring[normalize_index(cons_ci+i)]));
    }
    cons_advance(num);

    return true;
  }

  //! Deleted copy constructor
  SPSCQueue(const SPSCQueue &) = delete;
  //! Deleted copy assignment operator
  SPSCQueue &operator =(const SPSCQueue &) = delete;

  //! Deleted move constructor (class includes mutex)
  SPSCQueue(SPSCQueue &&) = delete;
  //! Deleted move assignment operator (class includes mutex)
  SPSCQueue &&operator =(SPSCQueue &&) = delete;

 private:
  //! Moves/Copy \p item to the front of the queue (producer)
  // This new template parameter U is necessary so the std::forward<U>()
  // used below can automatically deduce if item is an rvalue reference or
  // a const reference
  template <typename U>
  bool push_front_forward(U &&item, bool force) {
    prod_wait_space(1);
    // sure to have space
    ring[normalize_index(prod_pi)] = std::forward<U>(item);
    prod_advance(1, force);

    return true;
  }

  //! Used by the consumer to wait for 'want' elements.
  //  Returns number of available elements
  index_t cons_wait_data(index_t want) {
    index_t old = __cons_index;
    while (true) {
      if (cons_has_data(want)) { //queue not empty, go ahead
        break;
      }
      // sleep a while and retry
      std::this_thread::sleep_for(std::chrono::microseconds(cons_sleep_time));
      if (cons_has_data(want)) { //queue not empty, go ahead
        break;
      }
      // no data, request wake up when prod_index > cons_event
      cons_event = cons_ci + want - 1;
      cons_notify();
      if (cons_has_data(want)) { //double check
        break;
      }
      cons_sem.wait(); // finally, wait for notification
    }
    return cons_pi - cons_ci;
  }

  // used by consumer to update shared consumer index and signal the producer
  // if producer event is reached
  void cons_notify() {
    index_t old = __cons_index;
    // update shared consumer index
    __cons_index = cons_ci;

    index_t pe = prod_event;

    if (index_t(cons_ci - pe - 1) < index_t(cons_ci - old)) {
      prod_sem.signal();
      cons_not++;
    }
  }

  // used by consumer to advance its index and check if a notification is needed
  void cons_advance(index_t have) {
    cons_ci += have;
    if(cons_pi == cons_ci) {
      cons_notify();
    }
  }

  // used by the producer to wait until 'want' slots are available to fill
  // returns the number of available slots
  index_t prod_wait_space(index_t want) { // returns available space
    while (true) {
      if (prod_has_space(want)) {
        break;
      }
      // wake up when prod_c moves past prod_event (75% of current size)
      prod_event = index_t(prod_ci + index_t(prod_pi-prod_ci)/4);
      prod_notify();
      if (prod_has_space(want)) { // double check
        break;
      }
      // not enough space
      prod_sem.wait();
      prod_ci = __cons_index;
    }
    return prod_ci + queue_capacity - prod_pi;
  }

  // used by producer to update shared producer index and signal the consumer
  // if consumer event is reached
  void prod_notify() {
    index_t old = __prod_index;
    __prod_index = prod_pi;


    index_t ce = cons_event;

    if (index_t(prod_pi - ce - 1) < index_t(prod_pi - old)) {
      cons_sem.signal();
      prod_not++;
    }
  }

  // used by consumer to advance its index
  // update shared state and notify only if 'force' is true
  void prod_advance(index_t have, bool force) {
    prod_pi+=have;
    if (force) {
      prod_notify();
    }
  }

  // maps the index position in the ring to the actual array index
  size_t normalize_index(index_t index) {
    return index & (ring_capacity-1);
  }

  // check if 'want' slots are available for the producer
  bool prod_has_space(index_t want) {
    prod_ci = __cons_index;
    return (index_t(prod_pi-prod_ci) <= queue_capacity - want);
  }

  // check if 'want' elements are available for the consumer
  bool cons_has_data(index_t want) {
    cons_pi = __prod_index;
    return (index_t(cons_pi - cons_ci) >= want);
  }

private:
  size_t ring_capacity;
  size_t queue_capacity;
  std::unique_ptr<T[]> ring;

  alignas(64)
  atomic_index_t __prod_index{0}; // index of the next element to produce
  atomic_index_t prod_event{0}; // wake up when cons_index > prod_event
  index_t prod_ci{0}; // copy of consumer index (used by producer)
  index_t prod_pi{0}; // copy of producer index (used by producer)

  alignas(64)
  atomic_index_t __cons_index{0}; // index of the next element to consume
  atomic_index_t cons_event{0}; // wake up when prod_index > cons_event
  index_t cons_ci{0}; // copy of consumer index (used by consumer)
  index_t cons_pi{0}; // copy of producer index (used by consumer)

  alignas(64)
  Semaphore prod_sem;
  Semaphore cons_sem;

 public:
  uint64_t cons_not{0};
  uint64_t prod_not{0};
};

} // namespace bm

#endif // BM_BM_SIM_SPSC_QUEUE_H_
