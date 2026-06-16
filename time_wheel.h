#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>

namespace htw {

using TaskId = uint64_t;
using GroupId = uint64_t;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Millis = std::chrono::milliseconds;
using Micros = std::chrono::microseconds;
using Callback = std::function<void()>;

constexpr TaskId kInvalidTaskId = 0;
constexpr GroupId kDefaultGroup = 0;

enum class TaskState {
    Pending,
    Ready,
    Executing,
    Done,
    Cancelled
};

struct TaskNode {
    TaskId id{0};
    GroupId group{kDefaultGroup};
    Callback cb;
    TimePoint expire;
    Millis repeat_ms{0};
    TaskNode* next{nullptr};
    TaskNode* hash_next{nullptr};
    TaskNode* group_next{nullptr};
    TaskNode* group_prev{nullptr};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> executed{false};
    bool is_periodic{false};
};

class ThreadSafeQueue {
public:
    ThreadSafeQueue() = default;
    ~ThreadSafeQueue();

    void push(TaskNode* task);
    TaskNode* pop();
    void push_list(TaskNode* first, TaskNode* last);
    size_t approximate_size() const;

private:
    struct Node {
        TaskNode* task{nullptr};
        Node* next{nullptr};
        Node() = default;
        explicit Node(TaskNode* t) : task(t) {}
    };

    mutable std::mutex mtx_;
    Node* head_{nullptr};
    Node* tail_{nullptr};
    size_t size_{0};
};

class TaskNodePool {
public:
    TaskNodePool() = default;
    ~TaskNodePool();

    TaskNode* acquire();
    void release(TaskNode* n);
    size_t pool_size() const;

private:
    mutable std::mutex mtx_;
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
    TaskNode* find(TaskId id) const;
    void remove(TaskId id);
    size_t count() const;

private:
    mutable std::mutex mtx_;
    TaskNode* buckets_[kHashBuckets]{nullptr};
    size_t count_{0};
};

class GroupIndex {
public:
    GroupIndex();
    ~GroupIndex();

    void add(TaskNode* node);
    void remove(TaskNode* node);
    size_t cancel_group(GroupId group_id);
    size_t group_size(GroupId group_id) const;
    size_t total_groups() const;

private:
    mutable std::mutex mtx_;
    std::unordered_map<GroupId, TaskNode*> groups_;
    std::unordered_map<GroupId, size_t> group_counts_;
};

struct DelayHistogram {
    std::vector<int64_t> bounds_us;
    std::vector<uint64_t> counts;

    DelayHistogram();
    void record(int64_t delay_us);
    std::string to_string() const;
    int64_t p50() const;
    int64_t p99() const;
};

struct TimerStats {
    std::atomic<uint64_t> total_scheduled{0};
    std::atomic<uint64_t> total_executed{0};
    std::atomic<uint64_t> total_cancelled{0};
    std::atomic<uint64_t> cancel_attempts{0};
    std::atomic<uint64_t> periodic_scheduled{0};
    std::atomic<uint64_t> periodic_fires{0};

    std::atomic<uint64_t> queue_peak{0};
    std::atomic<uint64_t> tick_count{0};
    std::atomic<int64_t> last_tick_sec_tick{0};
    std::atomic<uint64_t> ticks_per_sec{0};

    DelayHistogram delay_hist;

    std::string to_string() const;
    void record_tick();
    void update_queue_peak(size_t sz);
};

class HierarchicalTimeWheel {
public:
    HierarchicalTimeWheel();
    ~HierarchicalTimeWheel();

    HierarchicalTimeWheel(const HierarchicalTimeWheel&) = delete;
    HierarchicalTimeWheel& operator=(const HierarchicalTimeWheel&) = delete;

    TaskId schedule(Callback cb, Millis delay);
    TaskId schedule_at(Callback cb, TimePoint when);
    TaskId schedule_periodic(Callback cb, Millis interval);
    TaskId schedule_group(Callback cb, Millis delay, GroupId group);
    TaskId schedule_periodic_group(Callback cb, Millis interval, GroupId group);

    bool cancel(TaskId id);
    size_t cancel_group(GroupId group_id);

    void shutdown();
    size_t pending_count() const;
    uint64_t current_tick() const;

    const TimerStats& stats() const;
    std::string stats_report() const;
    void save_stats_report(const std::string& path) const;

    bool task_exists(TaskId id) const;
    size_t group_size(GroupId group_id) const;
    size_t total_groups() const;

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
    TaskId schedule_internal(Callback cb, Millis delay, Millis repeat_ms, GroupId group);
    void handle_executed_task(TaskNode* node);

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

    ThreadSafeQueue ready_queue_;
    TaskNodePool pool_;
    TaskIndex task_index_;
    GroupIndex group_index_;
    mutable TimerStats stats_;

    std::thread tick_thread_;
    std::vector<std::thread> workers_;

    mutable std::mutex insert_mtx_;
    std::mutex stats_mtx_;
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

    TaskId schedule_periodic(Callback cb, Millis interval) {
        return impl_->schedule_periodic(std::move(cb), interval);
    }

    TaskId schedule_group(Callback cb, Millis delay, GroupId group) {
        return impl_->schedule_group(std::move(cb), delay, group);
    }

    TaskId schedule_periodic_group(Callback cb, Millis interval, GroupId group) {
        return impl_->schedule_periodic_group(std::move(cb), interval, group);
    }

    bool cancel(TaskId id) {
        return impl_->cancel(id);
    }

    size_t cancel_group(GroupId group_id) {
        return impl_->cancel_group(group_id);
    }

    size_t pending_count() const {
        return impl_->pending_count();
    }

    void shutdown() {
        impl_->shutdown();
    }

    const TimerStats& stats() const {
        return impl_->stats();
    }

    std::string stats_report() const {
        return impl_->stats_report();
    }

    void save_stats_report(const std::string& path) const {
        impl_->save_stats_report(path);
    }

    size_t group_size(GroupId group_id) const {
        return impl_->group_size(group_id);
    }

    size_t total_groups() const {
        return impl_->total_groups();
    }

private:
    std::unique_ptr<HierarchicalTimeWheel> impl_;
};

}  // namespace htw
