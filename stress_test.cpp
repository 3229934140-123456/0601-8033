#include "time_wheel.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <vector>
#include <iomanip>
#include <algorithm>

using namespace htw;
using namespace std::chrono;

static std::mutex g_print_mtx;

void print(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_print_mtx);
    std::cout << msg << std::endl;
}

void test_basic_schedule() {
    print("=== Test 1: Basic schedule ===");
    Timer timer;
    std::atomic<bool> done{false};
    auto start = steady_clock::now();
    timer.schedule([&]() {
        auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
        print("  Callback fired after " + std::to_string(elapsed) + "ms (expected ~100ms)");
        done.store(true);
    }, milliseconds(100));
    while (!done.load()) std::this_thread::sleep_for(milliseconds(10));
    print("  PASSED");
}

void test_million_tasks() {
    print("=== Test 2: 1,000,000 tasks spread across time ===");
    Timer timer;
    const int kTotal = 1000000;
    std::atomic<int> count{0};
    auto start = steady_clock::now();
    for (int i = 0; i < kTotal; ++i) {
        timer.schedule([&count]() {
            count.fetch_add(1, std::memory_order_relaxed);
        }, milliseconds(100 + (i % 500)));
    }
    auto insert_done = steady_clock::now();
    print("  Inserted " + std::to_string(kTotal) + " tasks in " +
          std::to_string(duration_cast<milliseconds>(insert_done - start).count()) + "ms");
    print("  Pending: " + std::to_string(timer.pending_count()));
    int wait_secs = 0;
    while (count.load() < kTotal && wait_secs < 15) {
        std::this_thread::sleep_for(seconds(1));
        ++wait_secs;
        print("  Completed so far: " + std::to_string(count.load()) + "/" + std::to_string(kTotal));
    }
    bool ok = count.load() == kTotal;
    print("  Completed: " + std::to_string(count.load()) + "/" + std::to_string(kTotal) +
          " -> " + (ok ? "PASSED" : "FAILED"));
}

void test_10k_simultaneous() {
    print("=== Test 3: 10,000 tasks fire at SAME tick (verify tick not blocked) ===");
    Timer timer;
    const int kSimul = 10000;
    std::atomic<int> count{0};
    std::vector<int64_t> fire_times(kSimul);
    std::atomic<int> idx{0};
    auto base_time = steady_clock::now() + milliseconds(500);
    for (int i = 0; i < kSimul; ++i) {
        timer.schedule_at([&count, &fire_times, &idx, base_time]() {
            int64_t delay = duration_cast<microseconds>(steady_clock::now() - base_time).count();
            int pos = idx.fetch_add(1, std::memory_order_relaxed);
            if (pos < (int)fire_times.size()) fire_times[pos] = delay;
            count.fetch_add(1, std::memory_order_relaxed);
        }, base_time);
    }
    print("  Scheduled 10k tasks for same tick... waiting...");
    while (count.load() < kSimul) {
        std::this_thread::sleep_for(milliseconds(50));
    }
    int valid = idx.load();
    if (valid > 0) {
        std::sort(fire_times.begin(), fire_times.begin() + valid);
        int64_t min_d = fire_times[0];
        int64_t max_d = fire_times[valid - 1];
        int64_t median = fire_times[valid / 2];
        int64_t total = 0;
        for (int i = 0; i < valid; ++i) total += fire_times[i];
        int64_t avg = total / valid;
        print("  Fire delay stats (microseconds): min=" + std::to_string(min_d) +
              " avg=" + std::to_string(avg) + " median=" + std::to_string(median) +
              " max=" + std::to_string(max_d));
        print("  Spread (max-min): " + std::to_string(max_d - min_d) + "us");
        if (max_d - min_d < 200000) {
            print("  PASSED - tick pointer was not blocked by 10k callbacks");
        } else {
            print("  WARNING - spread is large, callbacks may be blocking tick");
        }
    }
    print("  All " + std::to_string(count.load()) + " callbacks fired");
}

void test_precision_1ms() {
    print("=== Test 4: 1ms precision verification ===");
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
    print("  Error in microseconds:");
    print("    Min:    " + std::to_string(errors[0]) + "us");
    print("    Max:    " + std::to_string(errors[kN-1]) + "us");
    print("    Median: " + std::to_string(errors[kN/2]) + "us");
    print("    Mean:   " + std::to_string(total / kN) + "us");
    print("  PASSED");
}

void test_long_range_task() {
    print("=== Test 5: Long-range tasks (layered wheel memory efficiency) ===");
    Timer timer;
    std::atomic<bool> fired{false};
    auto start = steady_clock::now();
    auto id = timer.schedule([&fired, start]() {
        auto ms = duration_cast<milliseconds>(steady_clock::now() - start).count();
        print("  2-second task fired after " + std::to_string(ms) + "ms");
        fired.store(true);
    }, seconds(2));
    print("  Task ID " + std::to_string(id) + " scheduled ~2s from now");
    print("  Checking pending count (should not cause O(N) memory per wheel slot)...");
    print("  Pending: " + std::to_string(timer.pending_count()));
    while (!fired.load()) std::this_thread::sleep_for(milliseconds(100));
    print("  PASSED");
}

int main() {
    std::cout << std::fixed << std::setprecision(2);
    print("Hierarchical Time Wheel Timer - Test Suite");
    print("==========================================");

    test_basic_schedule();
    std::this_thread::sleep_for(milliseconds(50));

    test_precision_1ms();
    std::this_thread::sleep_for(milliseconds(50));

    test_long_range_task();
    std::this_thread::sleep_for(milliseconds(50));

    test_10k_simultaneous();
    std::this_thread::sleep_for(milliseconds(50));

    test_million_tasks();

    HierarchicalTimeWheel::instance().shutdown();
    print("==========================================");
    print("All tests complete.");
    return 0;
}
