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

static int g_pass = 0;
static int g_fail = 0;

void check(const std::string& name, bool pass) {
    if (pass) {
        ++g_pass;
        print("  [PASS] " + name);
    } else {
        ++g_fail;
        print("  [FAIL] " + name);
    }
}

int main() {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "==============================================================\n";
    std::cout << "  Hierarchical Time Wheel Timer - Full Feature Demo\n";
    std::cout << "==============================================================\n\n";

    // ========================================================================
    // Scenario 1: Basic delayed tasks
    // ========================================================================
    std::cout << "[1/6] Basic delayed tasks (100ms / 300ms / 500ms)\n";
    {
        Timer timer;
        std::atomic<int> done{0};
        auto t0 = steady_clock::now();
        for (int i = 0; i < 3; ++i) {
            int ms = 100 + i * 200;
            timer.schedule([i, ms, t0, &done]() {
                auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();
                print("    task " + std::to_string(i) + " sched@" + std::to_string(ms) +
                      "ms, fired@" + std::to_string(elapsed) + "ms");
                done.fetch_add(1);
            }, milliseconds(ms));
        }
        while (done.load() < 3) std::this_thread::sleep_for(milliseconds(10));
        check("all 3 basic tasks fired", done.load() == 3);
        timer.shutdown();
    }
    std::cout << "\n";

    // ========================================================================
    // Scenario 2: Cancel single task
    // ========================================================================
    std::cout << "[2/6] Cancel single task (schedule 2, cancel 1)\n";
    {
        Timer timer;
        std::atomic<int> fired{0};
        TaskId id_keep = timer.schedule([&fired]() {
            fired.fetch_add(1);
            print("    task A (kept) fired normally");
        }, milliseconds(200));
        TaskId id_cancel = timer.schedule([&fired]() {
            fired.fetch_add(1);
            print("    task B (cancelled) fired - WRONG!");
        }, milliseconds(200));
        bool ok = timer.cancel(id_cancel);
        print("    cancel(task B) = " + std::string(ok ? "true" : "false") +
              ", id_A=" + std::to_string(id_keep) + ", id_B=" + std::to_string(id_cancel));
        std::this_thread::sleep_for(milliseconds(400));
        print("    Actual fired: " + std::to_string(fired.load()) + " (expected 1)");
        check("cancel works correctly", fired.load() == 1 && ok);
        timer.shutdown();
    }
    std::cout << "\n";

    // ========================================================================
    // Scenario 3: Task groups and group cancel
    // ========================================================================
    std::cout << "[3/6] Task groups (schedule 50 in group 1 + 50 in group 2, cancel group 2)\n";
    {
        Timer timer;
        std::atomic<int> fired_g1{0};
        std::atomic<int> fired_g2{0};
        const int kPerGroup = 50;
        const GroupId kG1 = 1001;
        const GroupId kG2 = 2002;

        for (int i = 0; i < kPerGroup; ++i) {
            timer.schedule_group([&fired_g1]() { fired_g1.fetch_add(1); },
                                 milliseconds(200), kG1);
            timer.schedule_group([&fired_g2]() { fired_g2.fetch_add(1); },
                                 milliseconds(200), kG2);
        }

        print("    Before cancel: g1 size=" + std::to_string(timer.group_size(kG1)) +
              ", g2 size=" + std::to_string(timer.group_size(kG2)) +
              ", total groups=" + std::to_string(timer.total_groups()));

        size_t cancelled = timer.cancel_group(kG2);
        print("    cancel_group(" + std::to_string(kG2) + ") = " + std::to_string(cancelled) +
              " (expected " + std::to_string(kPerGroup) + ")");

        std::this_thread::sleep_for(milliseconds(500));
        print("    Results: g1 fired=" + std::to_string(fired_g1.load()) +
              ", g2 fired=" + std::to_string(fired_g2.load()));

        check("group 1 all fired", fired_g1.load() == kPerGroup);
        check("group 2 all cancelled (fired 0)", fired_g2.load() == 0);
        check("cancel_group returned correct count", cancelled == kPerGroup);
        timer.shutdown();
    }
    std::cout << "\n";

    // ========================================================================
    // Scenario 4: Periodic tasks
    // ========================================================================
    std::cout << "[4/6] Periodic tasks (10ms interval, run 10 times, then cancel)\n";
    {
        Timer timer;
        std::atomic<int> count{0};
        TaskId id = timer.schedule_periodic([&count]() {
            count.fetch_add(1);
        }, milliseconds(10));
        print("    Periodic task id=" + std::to_string(id) + " scheduled @ 10ms interval");

        int waited = 0;
        while (count.load() < 10 && waited < 200) {
            std::this_thread::sleep_for(milliseconds(20));
            waited += 20;
        }
        print("    After ~" + std::to_string(waited) + "ms: periodic fires = " +
              std::to_string(count.load()));

        bool ok = timer.cancel(id);
        int count_at_cancel = count.load();
        std::this_thread::sleep_for(milliseconds(100));
        int count_after = count.load();
        print("    Cancel: " + std::string(ok ? "ok" : "fail") +
              ", fires at cancel=" + std::to_string(count_at_cancel) +
              ", fires 100ms later=" + std::to_string(count_after));

        check("periodic fires multiple times", count_at_cancel >= 5);
        check("periodic stops after cancel", count_after <= count_at_cancel + 2);
        timer.shutdown();
    }
    std::cout << "\n";

    // ========================================================================
    // Scenario 5: Long-range cascade tasks (cross multiple wheel layers)
    // ========================================================================
    std::cout << "[5/6] Long-range cascade (tasks scheduled across wheel layers)\n";
    {
        Timer timer;
        std::atomic<bool> t1{false};
        std::atomic<bool> t2{false};
        std::atomic<bool> t3{false};
        auto t0 = steady_clock::now();

        print("    Scheduling 3 tasks that land in different wheel layers:");
        print("      300ms    -> wheel1 (64 x 256ms slots, beyond wheel0=256ms)");
        print("      1700ms   -> wheel2 (64 x 16s slots)");
        print("      20000ms  -> wheel3 (64 x 17min slots) - skipped in demo");

        timer.schedule([&t1, t0]() {
            auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
            print("    [wheel1] 300ms task fired @" + std::to_string(ms) + "ms");
            t1.store(true);
        }, milliseconds(300));

        timer.schedule([&t2, t0]() {
            auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
            print("    [wheel2] 1700ms task fired @" + std::to_string(ms) + "ms");
            t2.store(true);
        }, milliseconds(1700));

        print("    Pending count: " + std::to_string(timer.pending_count()) +
              "    (512 slots total, no matter how many tasks)");
        print("    Waiting for cascade... (wheel4->wheel3->wheel2->wheel1->wheel0)");

        int waited = 0;
        while ((!t1.load() || !t2.load()) && waited < 4000) {
            std::this_thread::sleep_for(milliseconds(100));
            waited += 100;
        }

        check("wheel1 task fired", t1.load());
        check("wheel2 task fired", t2.load());

        print("    Final pending: " + std::to_string(timer.pending_count()));
        print("    Note: 512 slots = 256 + 64*4, fixed size regardless of task count");
        timer.shutdown();
    }
    std::cout << "\n";

    // ========================================================================
    // Scenario 6: 10,000 tasks on same tick (tick not blocked by callbacks)
    // ========================================================================
    std::cout << "[6/6] 10k tasks on SAME tick (tick pointer not blocked by callbacks)\n";
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
        print("    Scheduled " + std::to_string(N) + " tasks at same tick...");

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
            print("    Delay stats (microseconds vs target):");
            print("      min=" + std::to_string(min_d) + "us, avg=" + std::to_string(avg) +
                  "us, max=" + std::to_string(max_d) + "us");
            print("      Spread (max-min): " + std::to_string(spread) + "us  (" +
                  std::to_string(spread / 1000) + "ms)");
            check("tick NOT blocked by 10k callbacks (spread < 500ms)",
                  spread < 500000);
        }
        timer.shutdown();
    }
    std::cout << "\n";

    // ========================================================================
    // Summary
    // ========================================================================
    std::cout << "==============================================================\n";
    std::cout << "  Results: " << g_pass << " passed, " << g_fail << " failed\n";
    std::cout << "==============================================================\n";

    return g_fail == 0 ? 0 : 1;
}
