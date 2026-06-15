#include "time_wheel.h"
#include <iostream>
#include <chrono>

using namespace htw;
using namespace std::chrono;

int main() {
    std::cout << "Hierarchical Time Wheel Timer - Basic Example\n" << std::endl;

    Timer timer;
    std::atomic<int> completed{0};

    timer.schedule([]() {
        std::cout << "[100ms] Hello from timer!" << std::endl;
    }, milliseconds(100));

    timer.schedule([&timer, &completed]() {
        std::cout << "[500ms] This one chains another task..." << std::endl;
        timer.schedule([&completed]() {
            std::cout << "[500ms + 30ms] Chained task executed" << std::endl;
            completed.fetch_add(1);
        }, milliseconds(30));
        completed.fetch_add(1);
    }, milliseconds(500));

    auto id = timer.schedule([]() {
        std::cout << "[1000ms] This should print" << std::endl;
    }, milliseconds(1000));
    (void)id;

    timer.schedule([&completed]() {
        std::cout << "[1500ms] Last task" << std::endl;
        completed.fetch_add(1);
    }, milliseconds(1500));

    std::cout << "Tasks scheduled. Pending: " << timer.pending_count() << std::endl;
    std::cout << "Waiting for tasks to fire...\n" << std::endl;

    while (completed.load() < 3) {
        std::this_thread::sleep_for(milliseconds(10));
    }
    std::this_thread::sleep_for(milliseconds(600));

    HierarchicalTimeWheel::instance().shutdown();
    std::cout << "\nDone." << std::endl;
    return 0;
}
