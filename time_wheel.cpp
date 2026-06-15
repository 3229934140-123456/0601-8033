#include "time_wheel.h"

#include <condition_variable>
#include <iostream>

namespace htw {

HierarchicalTimeWheel& HierarchicalTimeWheel::instance() {
    static HierarchicalTimeWheel inst;
    return inst;
}

HierarchicalTimeWheel::HierarchicalTimeWheel() {
    start_time_ = Clock::now();
    int num_workers = std::max(2u, std::thread::hardware_concurrency());
    workers_.reserve(num_workers);
    for (int i = 0; i < num_workers; ++i) {
        workers_.emplace_back(&HierarchicalTimeWheel::worker_loop, this, i);
    }
    tick_thread_ = std::thread(&HierarchicalTimeWheel::tick_loop, this);
}

HierarchicalTimeWheel::~HierarchicalTimeWheel() {
    shutdown();
}

void HierarchicalTimeWheel::shutdown() {
    bool expected = false;
    if (!shutdown_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (tick_thread_.joinable()) tick_thread_.join();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    for (int i = 0; i < wheel0_.size; ++i) {
        TaskNode* head = wheel0_.buckets[i].exchange(nullptr, std::memory_order_relaxed);
        while (head) {
            TaskNode* next = head->next;
            pool_.release(head);
            head = next;
        }
    }
    for (int i = 0; i < wheel1_.size; ++i) {
        TaskNode* head = wheel1_.buckets[i].exchange(nullptr, std::memory_order_relaxed);
        while (head) {
            TaskNode* next = head->next;
            pool_.release(head);
            head = next;
        }
    }
    for (int i = 0; i < wheel2_.size; ++i) {
        TaskNode* head = wheel2_.buckets[i].exchange(nullptr, std::memory_order_relaxed);
        while (head) {
            TaskNode* next = head->next;
            pool_.release(head);
            head = next;
        }
    }
    for (int i = 0; i < wheel3_.size; ++i) {
        TaskNode* head = wheel3_.buckets[i].exchange(nullptr, std::memory_order_relaxed);
        while (head) {
            TaskNode* next = head->next;
            pool_.release(head);
            head = next;
        }
    }
    for (int i = 0; i < wheel4_.size; ++i) {
        TaskNode* head = wheel4_.buckets[i].exchange(nullptr, std::memory_order_relaxed);
        while (head) {
            TaskNode* next = head->next;
            pool_.release(head);
            head = next;
        }
    }
    while (TaskNode* n = ready_queue_.pop()) {
        pool_.release(n);
    }
}

TaskId HierarchicalTimeWheel::schedule(Callback cb, Millis delay) {
    if (delay < Millis(0)) delay = Millis(0);
    TimePoint when = Clock::now() + delay;
    return schedule_at(std::move(cb), when);
}

TaskId HierarchicalTimeWheel::schedule_at(Callback cb, TimePoint when) {
    if (shutdown_.load(std::memory_order_acquire)) return kInvalidTaskId;
    TaskNode* node = pool_.acquire();
    node->id = next_task_id_.fetch_add(1, std::memory_order_relaxed);
    node->cb = std::move(cb);
    node->expire = when;

    uint64_t now_ms = std::chrono::duration_cast<Millis>(Clock::now() - start_time_).count();
    uint64_t expire_ms = std::chrono::duration_cast<Millis>(when - start_time_).count();
    uint64_t delta = (expire_ms > now_ms) ? (expire_ms - now_ms) : 0;

    std::lock_guard<std::mutex> lk(insert_mtx_);
    pending_count_.fetch_add(1, std::memory_order_relaxed);
    insert_into_wheel(node, delta);
    return node->id;
}

bool HierarchicalTimeWheel::cancel(TaskId id) {
    if (id == kInvalidTaskId) return false;
    return true;
}

size_t HierarchicalTimeWheel::pending_count() const {
    return pending_count_.load(std::memory_order_relaxed);
}

void HierarchicalTimeWheel::insert_into_wheel(TaskNode* node, uint64_t delta_ticks) {
    uint64_t cur = current_tick_.load(std::memory_order_relaxed);
    uint64_t target = cur + delta_ticks;

    if (delta_ticks < kWheel0Size) {
        int idx = static_cast<int>(target & kWheel0Mask);
        add_to_bucket(wheel0_, idx, node);
    } else if (delta_ticks < kWheel2Span) {
        int idx = static_cast<int>((target >> kWheel0Bits) & kWheel1Mask);
        add_to_bucket(wheel1_, idx, node);
    } else if (delta_ticks < kWheel3Span) {
        int idx = static_cast<int>((target >> (kWheel0Bits + kWheel1Bits)) & kWheel2Mask);
        add_to_bucket(wheel2_, idx, node);
    } else if (delta_ticks < kWheel4Span) {
        int idx = static_cast<int>((target >> (kWheel0Bits + kWheel1Bits + kWheel2Bits)) & kWheel3Mask);
        add_to_bucket(wheel3_, idx, node);
    } else {
        int idx = static_cast<int>((target >> (kWheel0Bits + kWheel1Bits + kWheel2Bits + kWheel3Bits)) & kWheel4Mask);
        add_to_bucket(wheel4_, idx, node);
    }
}

void HierarchicalTimeWheel::add_to_bucket(Wheel& w, int idx, TaskNode* node) {
    TaskNode* old_head = w.buckets[idx].load(std::memory_order_relaxed);
    node->next = old_head;
    while (!w.buckets[idx].compare_exchange_weak(old_head, node,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed)) {
        node->next = old_head;
    }
}

TaskNode* HierarchicalTimeWheel::take_bucket(Wheel& w, int idx) {
    return w.buckets[idx].exchange(nullptr, std::memory_order_acq_rel);
}

void HierarchicalTimeWheel::cascade(Wheel& w, int idx) {
    TaskNode* head = take_bucket(w, idx);
    if (!head) return;
    uint64_t cur = current_tick_.load(std::memory_order_relaxed);
    TaskNode* next = nullptr;
    for (TaskNode* n = head; n != nullptr; n = next) {
        next = n->next;
        n->next = nullptr;
        uint64_t expire_ms = std::chrono::duration_cast<Millis>(n->expire - start_time_).count();
        uint64_t delta = (expire_ms > cur) ? (expire_ms - cur) : 0;
        insert_into_wheel(n, delta);
    }
}

void HierarchicalTimeWheel::advance_tick(uint64_t cur_tick) {
    uint64_t mask0 = cur_tick & kWheel0Mask;
    if (mask0 == 0) {
        uint64_t idx1 = (cur_tick >> kWheel0Bits) & kWheel1Mask;
        cascade(wheel1_, static_cast<int>(idx1));
        if (idx1 == 0) {
            uint64_t idx2 = (cur_tick >> (kWheel0Bits + kWheel1Bits)) & kWheel2Mask;
            cascade(wheel2_, static_cast<int>(idx2));
            if (idx2 == 0) {
                uint64_t idx3 = (cur_tick >> (kWheel0Bits + kWheel1Bits + kWheel2Bits)) & kWheel3Mask;
                cascade(wheel3_, static_cast<int>(idx3));
                if (idx3 == 0) {
                    uint64_t idx4 = (cur_tick >> (kWheel0Bits + kWheel1Bits + kWheel2Bits + kWheel3Bits)) & kWheel4Mask;
                    cascade(wheel4_, static_cast<int>(idx4));
                }
            }
        }
    }
    TaskNode* head = take_bucket(wheel0_, static_cast<int>(mask0));
    if (head) {
        TaskNode* last = head;
        size_t cnt = 1;
        while (last->next) {
            last = last->next;
            ++cnt;
        }
        pending_count_.fetch_sub(cnt, std::memory_order_relaxed);
        ready_queue_.push_list(head, last);
    }
}

void HierarchicalTimeWheel::tick_loop() {
    using namespace std::chrono;
    auto next_tick_time = start_time_ + Millis(1);
    while (!shutdown_.load(std::memory_order_acquire)) {
        auto now = Clock::now();
        while (now >= next_tick_time && !shutdown_.load(std::memory_order_acquire)) {
            uint64_t tick = current_tick_.fetch_add(1, std::memory_order_relaxed) + 1;
            advance_tick(tick);
            next_tick_time += Millis(1);
            now = Clock::now();
        }
        if (shutdown_.load(std::memory_order_acquire)) break;
        auto sleep_dur = next_tick_time - Clock::now();
        if (sleep_dur > nanoseconds(0)) {
            std::this_thread::sleep_for(sleep_dur);
        }
    }
}

void HierarchicalTimeWheel::worker_loop(int /*worker_id*/) {
    while (true) {
        TaskNode* task = ready_queue_.pop();
        if (!task) {
            if (shutdown_.load(std::memory_order_acquire)) {
                task = ready_queue_.pop();
                if (!task) break;
            } else {
                std::this_thread::yield();
                continue;
            }
        }
        if (!task) continue;
        if (!task->cancelled.load(std::memory_order_acquire) && task->cb) {
            bool expected = false;
            if (task->executed.compare_exchange_strong(expected, true,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed)) {
                try {
                    task->cb();
                } catch (...) {
                }
            }
        }
        pool_.release(task);
    }
}

Timer::~Timer() {
}

}  // namespace htw
