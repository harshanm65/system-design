#include <coroutine>
#include <iostream>
#include <thread>
#include <chrono>

struct sleep_for {
    int ms;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const {
        std::thread([h, ms = ms]{
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            h.resume();
        }).detach();
    }
    void await_resume() const noexcept {}
};

struct task {
    struct promise_type {
        task get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

task run(const std::chrono::steady_clock::time_point& start) {
    std::cout << "Waiting...\n";
    co_await sleep_for{1000};
    auto end = std::chrono::steady_clock::now();
    std::cout << "Done! Elapsed: "
              << std::chrono::duration_cast<std::chrono::seconds>(end - start).count()
              << "s\n";
}

int main() {
    auto global_start = std::chrono::steady_clock::now();
    run(global_start);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto final_end = std::chrono::steady_clock::now();
    std::cout << "Total elapsed: "
              << std::chrono::duration_cast<std::chrono::seconds>(final_end - global_start).count()
              << "s\n";
}