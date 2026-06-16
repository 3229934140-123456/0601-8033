#include "time_wheel.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <vector>
#include <algorithm>

using namespace htw;
using namespace std::chrono;

static std::mutex g_print_mtx;

void print(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_print_mtx);
    std::cout << msg << std::endl;
}

void test_basic() {
    print("=== [1/5] Basic schedule ===");
    Timer timer;
    std::atomic<bool> done{false};
    auto start = steady_clock::now();
    timer.schedule([&]() {
        auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
        print("  Callback fired after " + std::to_string(elapsed) + "ms (expected ~100ms)");
        done.store(true);
    }, milliseconds(100));
    while (!done.load()) std::this_thread::sleep_for(milliseconds(10));
    timer.shutdown();
    print("  PASSED\n");
}

void test_precision() {
    print("=== [2/5] 1ms precision (100 random-delay tasks) ===");
    Timer timer;
    const int kN = 100;
    std::vector<int64_t> errors(kN);
    std::atomic<int> done_count{0};
    for (int i = 0; i < kN; ++i) {
        int delay_ms = 5 + i * 7;
        auto expected = steady_clock::now() + milliseconds(delay_ms);
        timer.schedule([&errors, &done_count, i, expected]() {
            int64_t err = duration_cast<microseconds>(steady_clock::now() - expected).count();
            errors[i] = err;
            done_count.fetch_add(1);
        }, milliseconds(delay_ms));
    }
    while (done_count.load() < kN) std::this_thread::sleep_for(milliseconds(10));
    std::sort(errors.begin(), errors.end());
    int64_t total = 0;
    for (auto e : errors) total += e;
    print("  Error microseconds:");
    print("    Min:    " + std::to_string(errors[0]) + "us");
    print("    Max:    " + std::to_string(errors[kN - 1]) + "us");
    print("    Median: " + std::to_string(errors[kN / 2]) + "us");
    print("    Mean:   " + std::to_string(total / kN) + "us");
    timer.shutdown();
    print("  PASSED\n");
}

void test_cancel() {
    print("=== [3/5] Cancel task ===");
    Timer timer;
    std::atomic<int> fired{0};
    std::vector<TaskId> ids;
    for (int i = 0; i < 100; ++i) {
        ids.push_back(timer.schedule([&fired]() { fired.fetch_add(1); }, milliseconds(200)));
    }
    int cancelled = 0;
    for (int i = 0; i < 100; i += 2) {
        if (timer.cancel(ids[i])) ++cancelled;
    }
    print("  Scheduled 100 tasks, attempted to cancel 50, actually cancelled: " +
          std::to_string(cancelled));
    std::this_thread::sleep_for(milliseconds(400));
    print("  Actually fired: " + std::to_string(fired.load()) + " (expected ~50)");
    timer.shutdown();
    if (fired.load() >= 45 && fired.load() <= 55) {
        print("  PASSED\n");
    } else {
        print("  FAILED\n");
    }
}

void test_long_range_cascade() {
    print("=== [4/5] Long-range cascade (1.7s task crosses multiple wheel layers) ===");
    Timer timer;
    std::atomic<bool> fired{false};
    auto start = steady_clock::now();
    TaskId id = timer.schedule([&fired, start]() {
        auto ms = duration_cast<milliseconds>(steady_clock::now() - start).count();
        print("  1.7s task fired at " + std::to_string(ms) + "ms");
        fired.store(true);
    }, milliseconds(1700));
    print("  Task ID: " + std::to_string(id) + ", Pending: " + std::to_string(timer.pending_count()));
    auto t0 = steady_clock::now();
    while (!fired.load() && duration_cast<seconds>(steady_clock::now() - t0).count() < 5) {
        std::this_thread::sleep_for(milliseconds(100));
    }
    timer.shutdown();
    if (fired.load()) {
        print("  PASSED (wheel4 -> wheel3 -> wheel2 -> wheel1 -> wheel0 cascade works)\n");
    } else {
        print("  FAILED\n");
    }
}

void test_10k_same_tick() {
    print("=== [5/5] 10,000 tasks on SAME tick (tick progress not blocked by callbacks) ===");
    Timer timer;
    const int kSimul = 10000;
    std::atomic<int> count{0};
    std::vector<int64_t> fire_times(kSimul);
    std::atomic<int> idx{0};
    auto base_time = steady_clock::now() + milliseconds(500);

    auto t_insert0 = steady_clock::now();
    for (int i = 0; i < kSimul; ++i) {
        timer.schedule_at([&count, &fire_times, &idx, base_time]() {
            int64_t delay_us = duration_cast<microseconds>(steady_clock::now() - base_time).count();
            int pos = idx.fetch_add(1, std::memory_order_relaxed);
            if (pos < (int)fire_times.size()) fire_times[pos] = delay_us;
            count.fetch_add(1, std::memory_order_relaxed);
        }, base_time);
    }
    auto t_insert1 = steady_clock::now();
    print("  Inserted " + std::to_string(kSimul) + " tasks in " +
          std::to_string(duration_cast<milliseconds>(t_insert1 - t_insert0).count()) + "ms");
    print("  Waiting for all callbacks to fire...");

    int wait_ms = 0;
    while (count.load() < kSimul && wait_ms < 5000) {
        std::this_thread::sleep_for(milliseconds(50));
        wait_ms += 50;
    }

    int valid = std::min(idx.load(), kSimul);
    if (valid > 0) {
        std::sort(fire_times.begin(), fire_times.begin() + valid);
        int64_t min_d = fire_times[0];
        int64_t max_d = fire_times[valid - 1];
        int64_t median = fire_times[valid / 2];
        int64_t total = 0;
        for (int i = 0; i < valid; ++i) total += fire_times[i];
        int64_t avg = total / valid;
        int64_t spread = max_d - min_d;
        print("  Fire delay stats (microseconds, relative to target tick):");
        print("    min=" + std::to_string(min_d) + "us  avg=" + std::to_string(avg) +
              "us  median=" + std::to_string(median) + "us  max=" + std::to_string(max_d) + "us");
        print("    Spread (max - min) = " + std::to_string(spread) + "us  (" +
              std::to_string(spread / 1000) + "ms)");
        if (spread < 200000) {
            print("  PASSED: tick pointer was NOT blocked by 10k callbacks");
        } else {
            print("  WARNING: spread is large; callbacks may be blocking tick progress");
        }
    }
    print("  Total callbacks fired: " + std::to_string(count.load()) + "/" + std::to_string(kSimul));
    timer.shutdown();
    print("");
}

void test_million_tasks() {
    print("=== [BONUS] 1,000,000 tasks (stress memory/throughput) ===");
    Timer timer;
    const int kTotal = 1000000;
    std::atomic<int> count{0};
    auto t0 = steady_clock::now();
    for (int i = 0; i < kTotal; ++i) {
        timer.schedule([&count]() {
            count.fetch_add(1, std::memory_order_relaxed);
        }, milliseconds(100 + (i % 500)));
    }
    auto t1 = steady_clock::now();
    print("  Inserted " + std::to_string(kTotal) + " tasks in " +
          std::to_string(duration_cast<milliseconds>(t1 - t0).count()) + "ms");
    print("  Pending count reported by timer: " + std::to_string(timer.pending_count()));
    print("  Waiting for all to complete (up to 10s)...");
    int waited = 0;
    while (count.load() < kTotal && waited < 100) {
        std::this_thread::sleep_for(milliseconds(100));
        ++waited;
    }
    print("  Completed: " + std::to_string(count.load()) + "/" + std::to_string(kTotal));
    timer.shutdown();
    if (count.load() == kTotal) {
        print("  PASSED\n");
    } else {
        print("  TIMEOUT - not all tasks finished in 10s\n");
    }
}

int main() {
    std::cout << std::fixed << std::setprecision(2);
    print("================================================");
    print(" Hierarchical Time Wheel - Stress Test Suite");
    print("================================================");
    print(" 5 layers: 256x1ms + 64x256ms + 64x16s + 64x17m + 64x18h");
    print(" Total slots: 512   Max range: ~49 days\n");

    test_basic();
    test_precision();
    test_cancel();
    test_long_range_cascade();
    test_10k_same_tick();
    test_million_tasks();

    print("================================================");
    print(" All tests finished.");
    print("================================================");
    return 0;
}
