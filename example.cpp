#include "time_wheel.h"
#include <iostream>
#include <iomanip>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>

using namespace htw;
using namespace std::chrono;

static std::mutex g_mtx;

void print(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::cout << s << std::endl << std::flush;
}

int main() {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "====================================================\n";
    std::cout << "  Hierarchical Time Wheel Timer - Demo\n";
    std::cout << "====================================================\n\n";

    // ========================================================================
    // Scenario 1: Basic delayed tasks
    // ========================================================================
    std::cout << "[Scenario 1] Basic delayed tasks (100ms, 300ms, 500ms)\n";
    {
        Timer timer;
        std::atomic<int> done{0};
        auto t0 = steady_clock::now();
        for (int i = 0; i < 3; ++i) {
            int ms = 100 + i * 200;
            timer.schedule([i, ms, t0, &done]() {
                auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();
                print("  task " + std::to_string(i) + " scheduled@" + std::to_string(ms) +
                      "ms, fired@" + std::to_string(elapsed) + "ms");
                done.fetch_add(1);
            }, milliseconds(ms));
        }
        while (done.load() < 3) std::this_thread::sleep_for(milliseconds(10));
        timer.shutdown();
    }
    std::cout << "  -> OK\n\n";

    // ========================================================================
    // Scenario 2: Cancel tasks
    // ========================================================================
    std::cout << "[Scenario 2] Cancel tasks (schedule 2 @200ms, cancel one)\n";
    {
        Timer timer;
        std::atomic<int> fired{0};
        TaskId id_keep = timer.schedule([&fired]() {
            fired.fetch_add(1);
            print("  task A (not cancelled) fired normally");
        }, milliseconds(200));
        TaskId id_cancel = timer.schedule([&fired]() {
            fired.fetch_add(1);
            print("  task B (cancelled) fired INCORRECTLY!!!");
        }, milliseconds(200));
        bool ok = timer.cancel(id_cancel);
        print("  cancel(task B) = " + std::string(ok ? "true" : "false") +
              ", id_A=" + std::to_string(id_keep) + ", id_B=" + std::to_string(id_cancel));
        std::this_thread::sleep_for(milliseconds(400));
        print("  Actual fired count: " + std::to_string(fired.load()) + " (expected 1)");
        timer.shutdown();
        std::cout << (fired.load() == 1 ? "  -> OK\n" : "  -> FAILED\n");
    }
    std::cout << "\n";

    // ========================================================================
    // Scenario 3: Long-range cascade (task scheduled in wheel2+, demoted down)
    // ========================================================================
    std::cout << "[Scenario 3] Long-range cascade (1700ms beyond wheel0=256ms, needs cascade)\n";
    {
        Timer timer;
        std::atomic<bool> fired{false};
        auto t0 = steady_clock::now();
        print("  Schedule a 1700ms task (lands on wheel2, demoted via wheel1->wheel0)");
        timer.schedule([&fired, t0]() {
            auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
            print("  Long-range task actually fired @" + std::to_string(ms) + "ms (expected ~1700ms)");
            fired.store(true);
        }, milliseconds(1700));
        print("  Waiting for cascade to trigger... (wheel2 -> wheel1 -> wheel0)");
        auto start = steady_clock::now();
        while (!fired.load() && duration_cast<seconds>(steady_clock::now() - start).count() < 5) {
            std::this_thread::sleep_for(milliseconds(100));
        }
        timer.shutdown();
        std::cout << (fired.load() ? "  -> OK\n" : "  -> FAILED\n");
    }
    std::cout << "\n";

    // ========================================================================
    // Scenario 4: 10000 tasks fire on the SAME tick (tick progress not blocked)
    // ========================================================================
    std::cout << "[Scenario 4] 10000 tasks on same tick (tick pointer NOT blocked by callbacks)\n";
    {
        Timer timer;
        const int N = 10000;
        std::atomic<int> count{0};
        std::vector<int64_t> delays(N);
        std::atomic<int> write_idx{0};

        auto base_time = steady_clock::now() + milliseconds(300);
        for (int i = 0; i < N; ++i) {
            timer.schedule_at([&count, &delays, &write_idx, base_time]() {
                int64_t us = duration_cast<microseconds>(steady_clock::now() - base_time).count();
                int pos = write_idx.fetch_add(1);
                if (pos < N) delays[pos] = us;
                count.fetch_add(1);
            }, base_time);
        }
        print("  Scheduled " + std::to_string(N) + " tasks all at same tick...");

        while (count.load() < N) std::this_thread::sleep_for(milliseconds(20));

        int valid = std::min(write_idx.load(), N);
        if (valid > 0) {
            std::sort(delays.begin(), delays.begin() + valid);
            int64_t min_d = delays[0];
            int64_t max_d = delays[valid - 1];
            int64_t spread = max_d - min_d;
            int64_t total = 0;
            for (int i = 0; i < valid; ++i) total += delays[i];
            int64_t avg = total / valid;
            print("  Delay distribution (microseconds vs target tick):");
            print("    earliest=" + std::to_string(min_d) + "us, avg=" + std::to_string(avg) +
                  "us, latest=" + std::to_string(max_d) + "us");
            print("    Overall spread (max - min): " + std::to_string(spread) + "us");
            if (spread < 200000) {
                print("  -> OK: 10k callbacks did NOT block tick pointer (spread < 200ms)");
            } else {
                print("  -> WARN: spread large; possible tick blocking");
            }
        }
        timer.shutdown();
    }

    std::cout << "\n====================================================\n";
    std::cout << "  All scenarios complete\n";
    std::cout << "====================================================\n";
    return 0;
}
