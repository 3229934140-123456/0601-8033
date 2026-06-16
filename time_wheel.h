#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <condition_variable>

namespace htw {

using TaskId = uint64_t;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Millis = std::chrono::milliseconds;
using Callback = std::function<void()>;

constexpr TaskId kInvalidTaskId = 0;

struct TaskNode {
    TaskId id{0};
    Callback cb;
    TimePoint expire;
    TaskNode* next{nullptr};
    TaskNode* hash_next{nullptr};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> executed{false};
};

class LockFreeQueue {
public:
    LockFreeQueue() = default;
    ~LockFreeQueue();

    void push(TaskNode* task);
    TaskNode* pop();
    void push_list(TaskNode* first, TaskNode* last);

private:
    struct Node {
        TaskNode* task{nullptr};
        Node* next{nullptr};
        Node() = default;
        explicit Node(TaskNode* t) : task(t) {}
    };

    std::mutex mtx_;
    Node* head_{nullptr};
    Node* tail_{nullptr};
};

class TaskNodePool {
public:
    TaskNodePool();
    ~TaskNodePool();

    TaskNode* acquire();
    void release(TaskNode* n);

private:
    std::mutex mtx_;
    std::vector<TaskNode*> free_list_;
};

constexpr int kWheel0Bits = 8;
constexpr int kWheel1Bits = 6;
constexpr int kWheel2Bits = 6;
constexpr int kWheel3Bits = 6;
constexpr int kWheel4Bits = 6;

constexpr int kWheel0Size = 1 << kWheel0Bits;
constexpr int kWheel1Size = 1 << kWheel1Bits;
constexpr int kWheel2Size = 1 << kWheel2Bits;
constexpr int kWheel3Size = 1 << kWheel3Bits;
constexpr int kWheel4Size = 1 << kWheel4Bits;

constexpr uint64_t kWheel0Mask = static_cast<uint64_t>(kWheel0Size - 1);
constexpr uint64_t kWheel1Mask = static_cast<uint64_t>(kWheel1Size - 1);
constexpr uint64_t kWheel2Mask = static_cast<uint64_t>(kWheel2Size - 1);
constexpr uint64_t kWheel3Mask = static_cast<uint64_t>(kWheel3Size - 1);
constexpr uint64_t kWheel4Mask = static_cast<uint64_t>(kWheel4Size - 1);

constexpr uint64_t kTickMs = 1;
constexpr uint64_t kWheel1Span = static_cast<uint64_t>(kWheel0Size);
constexpr uint64_t kWheel2Span = kWheel1Span * static_cast<uint64_t>(kWheel1Size);
constexpr uint64_t kWheel3Span = kWheel2Span * static_cast<uint64_t>(kWheel2Size);
constexpr uint64_t kWheel4Span = kWheel3Span * static_cast<uint64_t>(kWheel3Size);

constexpr int kHashBuckets = 4096;
constexpr uint64_t kHashMask = kHashBuckets - 1;

class TaskIndex {
public:
    TaskIndex();
    ~TaskIndex();

    void insert(TaskNode* node);
    TaskNode* find(TaskId id);
    void remove(TaskId id);

private:
    std::mutex mtx_;
    TaskNode* buckets_[kHashBuckets]{nullptr};
};

class HierarchicalTimeWheel {
public:
    HierarchicalTimeWheel();
    ~HierarchicalTimeWheel();

    HierarchicalTimeWheel(const HierarchicalTimeWheel&) = delete;
    HierarchicalTimeWheel& operator=(const HierarchicalTimeWheel&) = delete;

    TaskId schedule(Callback cb, Millis delay);
    TaskId schedule_at(Callback cb, TimePoint when);
    bool cancel(TaskId id);
    void shutdown();
    size_t pending_count() const;
    uint64_t current_tick() const;

private:
    struct Wheel {
        int size;
        std::atomic<TaskNode*>* buckets;
        explicit Wheel(int sz) : size(sz), buckets(new std::atomic<TaskNode*>[sz]) {
            for (int i = 0; i < sz; ++i) {
                buckets[i].store(nullptr, std::memory_order_relaxed);
            }
        }
        ~Wheel() { delete[] buckets; }
    };

    void tick_loop();
    void worker_loop();
    void advance_tick(uint64_t tick);
    void cascade(Wheel& w, int idx);
    void insert_into_wheel(TaskNode* node, uint64_t delta_ticks);
    void add_to_bucket(Wheel& w, int idx, TaskNode* node);
    TaskNode* take_bucket(Wheel& w, int idx);
    void release_bucket(TaskNode* head);

    Wheel wheel0_{kWheel0Size};
    Wheel wheel1_{kWheel1Size};
    Wheel wheel2_{kWheel2Size};
    Wheel wheel3_{kWheel3Size};
    Wheel wheel4_{kWheel4Size};

    std::atomic<uint64_t> current_tick_{0};
    std::atomic<uint64_t> next_task_id_{1};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> started_{false};
    std::atomic<size_t> pending_count_{0};

    TimePoint start_time_;

    LockFreeQueue ready_queue_;
    TaskNodePool pool_;
    TaskIndex task_index_;

    std::thread tick_thread_;
    std::vector<std::thread> workers_;

    mutable std::mutex insert_mtx_;
};

class Timer {
public:
    Timer();
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    TaskId schedule(Callback cb, Millis delay) {
        return impl_->schedule(std::move(cb), delay);
    }

    TaskId schedule_at(Callback cb, TimePoint when) {
        return impl_->schedule_at(std::move(cb), when);
    }

    bool cancel(TaskId id) {
        return impl_->cancel(id);
    }

    size_t pending_count() const {
        return impl_->pending_count();
    }

    void shutdown() {
        impl_->shutdown();
    }

private:
    std::unique_ptr<HierarchicalTimeWheel> impl_;
};

}  // namespace htw
