#include "time_wheel.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <algorithm>

namespace htw {

// ============================================================
// ThreadSafeQueue
// ============================================================

ThreadSafeQueue::~ThreadSafeQueue() {
    while (head_) {
        Node* n = head_;
        head_ = head_->next;
        delete n;
    }
    tail_ = nullptr;
}

void ThreadSafeQueue::push(TaskNode* task) {
    Node* new_node = new Node(task);
    std::lock_guard<std::mutex> lk(mtx_);
    if (tail_) {
        tail_->next = new_node;
        tail_ = new_node;
    } else {
        head_ = tail_ = new_node;
    }
    ++size_;
}

TaskNode* ThreadSafeQueue::pop() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!head_) return nullptr;
    Node* n = head_;
    head_ = n->next;
    if (!head_) tail_ = nullptr;
    TaskNode* task = n->task;
    n->task = nullptr;
    delete n;
    --size_;
    return task;
}

void ThreadSafeQueue::push_list(TaskNode* first, TaskNode* last) {
    if (!first) return;
    Node* list_head = nullptr;
    Node* list_tail = nullptr;
    size_t cnt = 0;
    for (TaskNode* t = first; t != nullptr; ) {
        TaskNode* tnext = t->next;
        t->next = nullptr;
        Node* n = new Node(t);
        if (!list_head) {
            list_head = list_tail = n;
        } else {
            list_tail->next = n;
            list_tail = n;
        }
        ++cnt;
        t = tnext;
    }
    if (!list_head) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (tail_) {
        tail_->next = list_head;
        tail_ = list_tail;
    } else {
        head_ = list_head;
        tail_ = list_tail;
    }
    size_ += cnt;
}

size_t ThreadSafeQueue::approximate_size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return size_;
}

// ============================================================
// TaskNodePool
// ============================================================

TaskNodePool::~TaskNodePool() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (TaskNode* n : free_list_) delete n;
}

TaskNode* TaskNodePool::acquire() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!free_list_.empty()) {
        TaskNode* n = free_list_.back();
        free_list_.pop_back();
        n->id = 0;
        n->group = kDefaultGroup;
        n->cb = nullptr;
        n->next = nullptr;
        n->hash_next = nullptr;
        n->group_next = nullptr;
        n->repeat_ms = Millis(0);
        n->is_periodic = false;
        n->cancelled.store(false, std::memory_order_relaxed);
        n->executed.store(false, std::memory_order_relaxed);
        return n;
    }
    return new TaskNode();
}

void TaskNodePool::release(TaskNode* n) {
    if (!n) return;
    n->cb = nullptr;
    n->hash_next = nullptr;
    n->group_next = nullptr;
    std::lock_guard<std::mutex> lk(mtx_);
    free_list_.push_back(n);
}

size_t TaskNodePool::pool_size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return free_list_.size();
}

// ============================================================
// TaskIndex
// ============================================================

TaskIndex::TaskIndex() = default;

TaskIndex::~TaskIndex() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (int i = 0; i < kHashBuckets; ++i) {
        TaskNode* n = buckets_[i];
        while (n) {
            TaskNode* next = n->hash_next;
            n->hash_next = nullptr;
            n = next;
        }
        buckets_[i] = nullptr;
    }
}

void TaskIndex::insert(TaskNode* node) {
    if (!node || node->id == kInvalidTaskId) return;
    uint64_t h = node->id & kHashMask;
    std::lock_guard<std::mutex> lk(mtx_);
    node->hash_next = buckets_[h];
    buckets_[h] = node;
    ++count_;
}

TaskNode* TaskIndex::find(TaskId id) const {
    if (id == kInvalidTaskId) return nullptr;
    uint64_t h = id & kHashMask;
    std::lock_guard<std::mutex> lk(mtx_);
    for (TaskNode* n = buckets_[h]; n; n = n->hash_next) {
        if (n->id == id) return n;
    }
    return nullptr;
}

void TaskIndex::remove(TaskId id) {
    if (id == kInvalidTaskId) return;
    uint64_t h = id & kHashMask;
    std::lock_guard<std::mutex> lk(mtx_);
    TaskNode** pp = &buckets_[h];
    while (*pp) {
        if ((*pp)->id == id) {
            TaskNode* found = *pp;
            *pp = found->hash_next;
            found->hash_next = nullptr;
            --count_;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

size_t TaskIndex::count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return count_;
}

// ============================================================
// GroupIndex
// ============================================================

GroupIndex::GroupIndex() = default;

GroupIndex::~GroupIndex() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& kv : groups_) {
        TaskNode* n = kv.second;
        while (n) {
            TaskNode* next = n->group_next;
            n->group_next = nullptr;
            n = next;
        }
    }
    groups_.clear();
    group_counts_.clear();
}

void GroupIndex::add(TaskNode* node) {
    if (!node) return;
    std::lock_guard<std::mutex> lk(mtx_);
    node->group_prev = nullptr;
    node->group_next = groups_[node->group];
    if (groups_[node->group]) {
        groups_[node->group]->group_prev = node;
    }
    groups_[node->group] = node;
    group_counts_[node->group]++;
}

void GroupIndex::remove(TaskNode* node) {
    if (!node) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (node->group_prev) {
        node->group_prev->group_next = node->group_next;
    } else {
        auto it = groups_.find(node->group);
        if (it == groups_.end()) return;
        if (it->second != node) return;
        groups_[node->group] = node->group_next;
    }
    if (node->group_next) {
        node->group_next->group_prev = node->group_prev;
    }
    node->group_prev = nullptr;
    node->group_next = nullptr;
    auto cnt_it = group_counts_.find(node->group);
    if (cnt_it == group_counts_.end()) return;
    if (--cnt_it->second == 0) {
        groups_.erase(node->group);
        group_counts_.erase(node->group);
    }
}

size_t GroupIndex::cancel_group(GroupId group_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return 0;
    size_t cnt = 0;
    TaskNode* n = it->second;
    while (n) {
        if (!n->cancelled.exchange(true, std::memory_order_acq_rel)) {
            ++cnt;
        }
        n = n->group_next;
    }
    return cnt;
}

size_t GroupIndex::group_size(GroupId group_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = group_counts_.find(group_id);
    if (it == group_counts_.end()) return 0;
    return it->second;
}

size_t GroupIndex::total_groups() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return groups_.size();
}

// ============================================================
// DelayHistogram
// ============================================================

DelayHistogram::DelayHistogram() {
    bounds_us = {
        0, 10, 50, 100, 250, 500, 1000, 2000, 5000,
        10000, 20000, 50000, 100000, 200000, 500000, 1000000
    };
    counts.resize(bounds_us.size(), 0);
}

void DelayHistogram::record(int64_t delay_us) {
    for (size_t i = 0; i < bounds_us.size(); ++i) {
        if (delay_us <= bounds_us[i]) {
            counts[i]++;
            return;
        }
    }
    if (!counts.empty()) counts.back()++;
}

int64_t DelayHistogram::p50() const {
    uint64_t total = 0;
    for (auto c : counts) total += c;
    if (total == 0) return 0;
    uint64_t target = total / 2;
    uint64_t acc = 0;
    for (size_t i = 0; i < bounds_us.size(); ++i) {
        acc += counts[i];
        if (acc >= target) return bounds_us[i];
    }
    return bounds_us.empty() ? 0 : bounds_us.back();
}

int64_t DelayHistogram::p99() const {
    uint64_t total = 0;
    for (auto c : counts) total += c;
    if (total == 0) return 0;
    uint64_t target = total * 99 / 100;
    uint64_t acc = 0;
    for (size_t i = 0; i < bounds_us.size(); ++i) {
        acc += counts[i];
        if (acc >= target) return bounds_us[i];
    }
    return bounds_us.empty() ? 0 : bounds_us.back();
}

std::string DelayHistogram::to_string() const {
    std::ostringstream oss;
    oss << "  Delay distribution (microseconds):\n";
    for (size_t i = 0; i < bounds_us.size(); ++i) {
        if (counts[i] == 0) continue;
        oss << "    <= " << std::setw(8) << bounds_us[i] << " us : "
            << std::setw(8) << counts[i] << "\n";
    }
    oss << "    p50=" << p50() << " us,  p99=" << p99() << " us";
    return oss.str();
}

// ============================================================
// TimerStats
// ============================================================

void TimerStats::record_tick() {
    tick_count.fetch_add(1, std::memory_order_relaxed);
}

void TimerStats::update_queue_peak(size_t sz) {
    uint64_t cur = queue_peak.load(std::memory_order_relaxed);
    while (sz > cur) {
        if (queue_peak.compare_exchange_weak(cur, sz,
                                             std::memory_order_relaxed)) {
            break;
        }
    }
}

static std::string fmt_num(uint64_t n) {
    std::string s = std::to_string(n);
    int len = (int)s.size();
    for (int i = len - 3; i > 0; i -= 3) {
        s.insert(i, ",");
    }
    return s;
}

std::string TimerStats::to_string() const {
    std::ostringstream oss;
    oss << std::left;
    oss << "==========================================================\n";
    oss << "  Hierarchical Time Wheel - Statistics Report\n";
    oss << "==========================================================\n\n";

    oss << "  ┌──────────────────────────────────────────────┐\n";
    oss << "  │ Metric                          │ Value                    │\n";
    oss << "  ├──────────────────────────────────────────────┤\n";
    oss << "  │ Total scheduled                │ " << std::setw(24) << fmt_num(total_scheduled.load()) << " │\n";
    oss << "  │ Total executed                 │ " << std::setw(24) << fmt_num(total_executed.load()) << " │\n";
    oss << "  │ Total cancelled             │ " << std::setw(24) << fmt_num(total_cancelled.load()) << " │\n";
    oss << "  │ Cancel attempts           │ " << std::setw(24) << fmt_num(cancel_attempts.load()) << " │\n";

    uint64_t ca = cancel_attempts.load();
    uint64_t tc = total_cancelled.load();
    double hit_rate = ca > 0 ? (double)tc / ca * 100.0 : 0.0;
    oss << "  │ Cancel hit rate           │ " << std::setw(21) << std::fixed << std::setprecision(2) << hit_rate << "%  │\n";
    oss << "  │ Periodic tasks scheduled  │ " << std::setw(24) << fmt_num(periodic_scheduled.load()) << " │\n";
    oss << "  │ Periodic fires total      │ " << std::setw(24) << fmt_num(periodic_fires.load()) << " │\n";
    oss << "  ├──────────────────────────────────────────────┤\n";

    oss << "  │ Tick count                    │ " << std::setw(24) << fmt_num(tick_count.load()) << " │\n";
    oss << "  │ Ticks/sec (instant)     │ " << std::setw(24) << fmt_num(ticks_per_sec.load()) << " │\n";
    oss << "  │ Ready queue peak          │ " << std::setw(24) << fmt_num(queue_peak.load()) << " │\n";
    oss << "  └──────────────────────────────────────────────┘\n\n";

    oss << delay_hist.to_string() << "\n";
    oss << "==========================================================\n";
    return oss.str();
}

// ============================================================
// HierarchicalTimeWheel
// ============================================================

HierarchicalTimeWheel::HierarchicalTimeWheel() {
    start_time_ = Clock::now();
    unsigned hw = std::thread::hardware_concurrency();
    int num_workers = std::max(2u, hw);
    workers_.reserve(num_workers);
    for (int i = 0; i < num_workers; ++i) {
        workers_.emplace_back(&HierarchicalTimeWheel::worker_loop, this);
    }
    started_.store(true, std::memory_order_release);
    tick_thread_ = std::thread(&HierarchicalTimeWheel::tick_loop, this);
}

HierarchicalTimeWheel::~HierarchicalTimeWheel() {
    shutdown();
}

void HierarchicalTimeWheel::release_bucket(TaskNode* head) {
    while (head) {
        TaskNode* next = head->next;
        head->next = nullptr;
        if (head->id != kInvalidTaskId) {
            task_index_.remove(head->id);
            group_index_.remove(head);
        }
        pool_.release(head);
        head = next;
    }
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
        release_bucket(head);
    }
    for (int i = 0; i < wheel1_.size; ++i) {
        TaskNode* head = wheel1_.buckets[i].exchange(nullptr, std::memory_order_relaxed);
        release_bucket(head);
    }
    for (int i = 0; i < wheel2_.size; ++i) {
        TaskNode* head = wheel2_.buckets[i].exchange(nullptr, std::memory_order_relaxed);
        release_bucket(head);
    }
    for (int i = 0; i < wheel3_.size; ++i) {
        TaskNode* head = wheel3_.buckets[i].exchange(nullptr, std::memory_order_relaxed);
        release_bucket(head);
    }
    for (int i = 0; i < wheel4_.size; ++i) {
        TaskNode* head = wheel4_.buckets[i].exchange(nullptr, std::memory_order_relaxed);
        release_bucket(head);
    }
    while (TaskNode* n = ready_queue_.pop()) {
        if (n->id != kInvalidTaskId) {
            task_index_.remove(n->id);
            group_index_.remove(n);
        }
        pool_.release(n);
    }
}

TaskId HierarchicalTimeWheel::schedule_internal(Callback cb, Millis delay, Millis repeat_ms, GroupId group) {
    if (shutdown_.load(std::memory_order_acquire)) return kInvalidTaskId;
    if (delay < Millis(0)) delay = Millis(0);
    TimePoint when = Clock::now() + delay;

    TaskNode* node = pool_.acquire();
    node->id = next_task_id_.fetch_add(1, std::memory_order_relaxed);
    node->cb = std::move(cb);
    node->expire = when;
    node->group = group;
    node->repeat_ms = repeat_ms;
    node->is_periodic = (repeat_ms > Millis(0));

    uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<Millis>(Clock::now() - start_time_).count());
    uint64_t expire_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<Millis>(when - start_time_).count());
    uint64_t delta = (expire_ms > now_ms) ? (expire_ms - now_ms) : 0;

    task_index_.insert(node);
    group_index_.add(node);

    {
        std::lock_guard<std::mutex> lk(insert_mtx_);
        pending_count_.fetch_add(1, std::memory_order_relaxed);
        insert_into_wheel(node, delta);
    }

    stats_.total_scheduled.fetch_add(1, std::memory_order_relaxed);
    if (node->is_periodic) {
        stats_.periodic_scheduled.fetch_add(1, std::memory_order_relaxed);
    }
    return node->id;
}

TaskId HierarchicalTimeWheel::schedule(Callback cb, Millis delay) {
    return schedule_internal(std::move(cb), delay, Millis(0), kDefaultGroup);
}

TaskId HierarchicalTimeWheel::schedule_at(Callback cb, TimePoint when) {
    auto delay = std::chrono::duration_cast<Millis>(when - Clock::now());
    return schedule_internal(std::move(cb), delay, Millis(0), kDefaultGroup);
}

TaskId HierarchicalTimeWheel::schedule_periodic(Callback cb, Millis interval) {
    return schedule_internal(std::move(cb), interval, interval, kDefaultGroup);
}

TaskId HierarchicalTimeWheel::schedule_group(Callback cb, Millis delay, GroupId group) {
    return schedule_internal(std::move(cb), delay, Millis(0), group);
}

TaskId HierarchicalTimeWheel::schedule_periodic_group(Callback cb, Millis interval, GroupId group) {
    return schedule_internal(std::move(cb), interval, interval, group);
}

bool HierarchicalTimeWheel::cancel(TaskId id) {
    if (id == kInvalidTaskId) return false;
    stats_.cancel_attempts.fetch_add(1, std::memory_order_relaxed);
    TaskNode* node = task_index_.find(id);
    if (!node) return false;
    bool was_already = node->cancelled.exchange(true, std::memory_order_acq_rel);
    if (!was_already) {
        stats_.total_cancelled.fetch_add(1, std::memory_order_relaxed);
    }
    return !was_already;
}

size_t HierarchicalTimeWheel::cancel_group(GroupId group_id) {
    size_t n = group_index_.cancel_group(group_id);
    stats_.cancel_attempts.fetch_add(n, std::memory_order_relaxed);
    stats_.total_cancelled.fetch_add(n, std::memory_order_relaxed);
    return n;
}

size_t HierarchicalTimeWheel::pending_count() const {
    return pending_count_.load(std::memory_order_relaxed);
}

uint64_t HierarchicalTimeWheel::current_tick() const {
    return current_tick_.load(std::memory_order_relaxed);
}

bool HierarchicalTimeWheel::task_exists(TaskId id) const {
    return task_index_.find(id) != nullptr;
}

size_t HierarchicalTimeWheel::group_size(GroupId group_id) const {
    return group_index_.group_size(group_id);
}

size_t HierarchicalTimeWheel::total_groups() const {
    return group_index_.total_groups();
}

const TimerStats& HierarchicalTimeWheel::stats() const {
    return stats_;
}

std::string HierarchicalTimeWheel::stats_report() const {
    return stats_.to_string();
}

void HierarchicalTimeWheel::save_stats_report(const std::string& path) const {
    std::ofstream ofs(path);
    if (ofs) {
        ofs << stats_report();
    }
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
    std::lock_guard<std::mutex> lk(insert_mtx_);
    uint64_t cur = current_tick_.load(std::memory_order_relaxed);
    TaskNode* next = nullptr;
    for (TaskNode* n = head; n != nullptr; n = next) {
        next = n->next;
        n->next = nullptr;
        uint64_t expire_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<Millis>(n->expire - start_time_).count());
        uint64_t delta = (expire_ms > cur) ? (expire_ms - cur) : 0;
        insert_into_wheel(n, delta);
    }
}

void HierarchicalTimeWheel::advance_tick(uint64_t tick) {
    stats_.record_tick();

    uint64_t mask0 = tick & kWheel0Mask;
    if (mask0 == 0) {
        uint64_t idx1 = ((tick >> kWheel0Bits) & kWheel1Mask);
        cascade(wheel1_, static_cast<int>(idx1));
        if (idx1 == 0) {
            uint64_t idx2 = ((tick >> (kWheel0Bits + kWheel1Bits)) & kWheel2Mask);
            cascade(wheel2_, static_cast<int>(idx2));
            if (idx2 == 0) {
                uint64_t idx3 = ((tick >> (kWheel0Bits + kWheel1Bits + kWheel2Bits)) & kWheel3Mask);
                cascade(wheel3_, static_cast<int>(idx3));
                if (idx3 == 0) {
                    uint64_t idx4 = ((tick >> (kWheel0Bits + kWheel1Bits + kWheel2Bits + kWheel3Bits)) & kWheel4Mask);
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
        size_t qsz = ready_queue_.approximate_size();
        stats_.update_queue_peak(qsz);
    }
}

void HierarchicalTimeWheel::tick_loop() {
    using namespace std::chrono;
    auto next_tick_time = start_time_ + Millis(1);
    auto last_sec = start_time_;
    uint64_t last_sec_ticks = 0;

    while (!shutdown_.load(std::memory_order_acquire)) {
        auto now = Clock::now();
        while (now >= next_tick_time && !shutdown_.load(std::memory_order_acquire)) {
            uint64_t tick = current_tick_.fetch_add(1, std::memory_order_relaxed) + 1;
            advance_tick(tick);
            next_tick_time += Millis(1);
            ++last_sec_ticks;
            now = Clock::now();

            if (now - last_sec >= seconds(1)) {
                stats_.ticks_per_sec.store(last_sec_ticks, std::memory_order_relaxed);
                last_sec_ticks = 0;
                last_sec = now;
            }
        }
        if (shutdown_.load(std::memory_order_acquire)) break;
        auto sleep_dur = next_tick_time - Clock::now();
        if (sleep_dur > nanoseconds(0)) {
            std::this_thread::sleep_for(sleep_dur);
        }
    }
}

void HierarchicalTimeWheel::handle_executed_task(TaskNode* node) {
    if (!node) return;

    stats_.total_executed.fetch_add(1, std::memory_order_relaxed);

    if (node->is_periodic && !node->cancelled.load(std::memory_order_acquire)) {
        stats_.periodic_fires.fetch_add(1, std::memory_order_relaxed);

        node->executed.store(false, std::memory_order_relaxed);
        node->expire = Clock::now() + node->repeat_ms;

        uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<Millis>(Clock::now() - start_time_).count());
        uint64_t expire_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<Millis>(node->expire - start_time_).count());
        uint64_t delta = (expire_ms > now_ms) ? (expire_ms - now_ms) : 0;

        {
            std::lock_guard<std::mutex> lk(insert_mtx_);
            pending_count_.fetch_add(1, std::memory_order_relaxed);
            insert_into_wheel(node, delta);
        }
        stats_.total_scheduled.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    task_index_.remove(node->id);
    group_index_.remove(node);
    pool_.release(node);
}

void HierarchicalTimeWheel::worker_loop() {
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

        bool do_cb = false;
        TimePoint expected_expire;
        if (!task->cancelled.load(std::memory_order_acquire) && task->cb) {
            bool expected = false;
            if (task->executed.compare_exchange_strong(expected, true,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed)) {
                do_cb = true;
                expected_expire = task->expire;
            }
        }

        if (do_cb) {
            try {
                task->cb();
            } catch (...) {
            }
            int64_t delay_us = std::chrono::duration_cast<Micros>(
                Clock::now() - expected_expire).count();
            stats_.delay_hist.record(delay_us);
        }

        handle_executed_task(task);
    }
}

// ============================================================
// Timer
// ============================================================

Timer::Timer() : impl_(std::make_unique<HierarchicalTimeWheel>()) {
}

Timer::~Timer() {
    if (impl_) impl_->shutdown();
}

}  // namespace htw
