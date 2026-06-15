#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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
    std::atomic<bool> cancelled{false};
    std::atomic<bool> executed{false};
};

class LockFreeQueue {
public:
    LockFreeQueue() {
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    ~LockFreeQueue() {
        while (Node* n = head_.load(std::memory_order_relaxed)) {
            head_.store(n->next, std::memory_order_relaxed);
            delete n;
        }
    }

    void push(TaskNode* task) {
        Node* new_node = new Node(task);
        Node* old_tail = tail_.exchange(new_node, std::memory_order_acq_rel);
        old_tail->next = new_node;
    }

    TaskNode* pop() {
        Node* old_head = head_.load(std::memory_order_relaxed);
        Node* next = old_head->next;
        if (next == nullptr) return nullptr;
        head_.store(next, std::memory_order_release);
        TaskNode* task = next->task;
        next->task = nullptr;
        delete old_head;
        return task;
    }

    void push_list(TaskNode* first, TaskNode* last) {
        if (!first) return;
        Node* cur_first = nullptr;
        Node* cur_last = nullptr;
        for (TaskNode* t = first; t != nullptr; ) {
            TaskNode* next = t->next;
            t->next = nullptr;
            Node* n = new Node(t);
            if (!cur_first) {
                cur_first = n;
                cur_last = n;
            } else {
                cur_last->next = n;
                cur_last = n;
            }
            t = next;
        }
        if (cur_first) {
            Node* old_tail = tail_.exchange(cur_last, std::memory_order_acq_rel);
            old_tail->next = cur_first;
        }
    }

private:
    struct Node {
        TaskNode* task{nullptr};
        Node* next{nullptr};
        Node() = default;
        explicit Node(TaskNode* t) : task(t) {}
    };

    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
};

class TaskNodePool {
public:
    TaskNodePool() = default;
    ~TaskNodePool() {
        std::lock_guard<std::mutex> lk(mtx_);
        for (TaskNode* n : free_list_) delete n;
    }

    TaskNode* acquire() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!free_list_.empty()) {
            TaskNode* n = free_list_.back();
            free_list_.pop_back();
            n->id = 0;
            n->cb = nullptr;
            n->next = nullptr;
            n->cancelled.store(false, std::memory_order_relaxed);
            n->executed.store(false, std::memory_order_relaxed);
            return n;
        }
        return new TaskNode();
    }

    void release(TaskNode* n) {
        if (!n) return;
        n->cb = nullptr;
        std::lock_guard<std::mutex> lk(mtx_);
        free_list_.push_back(n);
    }

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

constexpr uint64_t kWheel0Mask = kWheel0Size - 1;
constexpr uint64_t kWheel1Mask = kWheel1Size - 1;
constexpr uint64_t kWheel2Mask = kWheel2Size - 1;
constexpr uint64_t kWheel3Mask = kWheel3Size - 1;
constexpr uint64_t kWheel4Mask = kWheel4Size - 1;

constexpr uint64_t kTickMs = 1;
constexpr uint64_t kWheel1Span = kWheel0Size;
constexpr uint64_t kWheel2Span = kWheel1Span * kWheel1Size;
constexpr uint64_t kWheel3Span = kWheel2Span * kWheel2Size;
constexpr uint64_t kWheel4Span = kWheel3Span * kWheel3Size;

class HierarchicalTimeWheel {
public:
    static HierarchicalTimeWheel& instance();

    HierarchicalTimeWheel();
    ~HierarchicalTimeWheel();

    HierarchicalTimeWheel(const HierarchicalTimeWheel&) = delete;
    HierarchicalTimeWheel& operator=(const HierarchicalTimeWheel&) = delete;

    TaskId schedule(Callback cb, Millis delay);
    TaskId schedule_at(Callback cb, TimePoint when);
    bool cancel(TaskId id);
    void shutdown();
    size_t pending_count() const;

private:
    struct Wheel {
        int size;
        std::atomic<TaskNode*> buckets;
        explicit Wheel(int sz) : size(sz), buckets(new std::atomic<TaskNode*>[sz]) {
            for (int i = 0; i < sz; ++i) {
                buckets[i].store(nullptr, std::memory_order_relaxed);
            }
        }
        ~Wheel() { delete[] buckets; }
    };

    void tick_loop();
    void worker_loop(int worker_id);
    void advance_tick(uint64_t cur_tick);
    void cascade(Wheel& w, int idx);
    void insert_into_wheel(TaskNode* node, uint64_t delta_ticks);
    void add_to_bucket(Wheel& w, int idx, TaskNode* node);
    TaskNode* take_bucket(Wheel& w, int idx);

    Wheel wheel0_{kWheel0Size};
    Wheel wheel1_{kWheel1Size};
    Wheel wheel2_{kWheel2Size};
    Wheel wheel3_{kWheel3Size};
    Wheel wheel4_{kWheel4Size};

    std::atomic<uint64_t> current_tick_{0};
    std::atomic<uint64_t> next_task_id_{1};
    std::atomic<bool> shutdown_{false};
    std::atomic<size_t> pending_count_{0};

    TimePoint start_time_;

    LockFreeQueue ready_queue_;
    TaskNodePool pool_;

    std::thread tick_thread_;
    std::vector<std::thread> workers_;

    mutable std::mutex insert_mtx_;
};

class Timer {
public:
    Timer() = default;
    ~Timer();

    TaskId schedule(Callback cb, Millis delay) {
        return HierarchicalTimeWheel::instance().schedule(std::move(cb), delay);
    }

    TaskId schedule_at(Callback cb, TimePoint when) {
        return HierarchicalTimeWheel::instance().schedule_at(std::move(cb), when);
    }

    bool cancel(TaskId id) {
        return HierarchicalTimeWheel::instance().cancel(id);
    }

    size_t pending_count() const {
        return HierarchicalTimeWheel::instance().pending_count();
    }
};

}  // namespace htw
