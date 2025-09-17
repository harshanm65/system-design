#include <coroutine>
#include <iostream>
#include <optional>

template<typename T>
struct generator {
    struct promise_type {
        T current_value;
        T value_one, value_two;
        auto get_return_object() { return generator{handle_type::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void unhandled_exception() { std::exit(1); }
        std::suspend_always yield_value(T v) noexcept {
            current_value = v;
            return {};
        }
        void return_void() {}
    };
    using handle_type = std::coroutine_handle<promise_type>;
    handle_type h;
    generator(handle_type h): h(h) {}
    ~generator(){ if(h) h.destroy(); }

    std::optional<T> next() {
        if(!h.done()) {
            h.resume();
            if (h.done())
                return std::nullopt;
            return h.promise().current_value;
        }
        return std::nullopt;
    }
};

generator<int> numbers() {
    // for(int i=1;i<=3;i++) co_yield i;
    co_yield 1;
    co_yield 2;
    co_yield 3;
}

generator<int> fibonacci_numbers() {

}

int main() {
    generator<int> g1 = numbers();
    generator<int>& g = g1;
    while(auto v = g.next())
        std::cout << *v << "\n";
    return 0;
}
