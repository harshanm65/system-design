#include <atomic>
#include <coroutine>
#include <future>
#include <iostream>
#include <vector>
#include <utility>
#include <thread>
#include <optional>
#include <mutex>
#include <memory>
#include <string>
#include <queue>
#include <deque>
#include <mutex>
#include <deque>

template <class T>
class async_channel {
public:
    void push(T v) {

    }
};

struct FileState {
    std::string path;
    std::streampos off = 0;
    bool done = false;
};

struct read_chunk {
    std::string path;
    std::streampos off;
    std::size_t n;
    std::shared_ptr<std::string> out = std::make_shared<std::string>();

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        std::thread([path = path, off = off, n = n, out = out, h] {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                *out = {};
                h.resume();
                return;
            }

            in.seekg(off);
            std::string buf;
            buf.resize(n);

            in.read(buf.data(), static_cast<std::streamsize>(n));
            buf.resize(static_cast<std::size_t>(in.gcount()));

            *out = std::move(buf);
            h.resume();
        }).detach();
    }

    std::string await_resume() const noexcept {
        return std::move(*out);
    }
};

template<class T>
struct task {
    struct promise_type {
        std::optional<T> val;
        std::exception_ptr ep;
        std::coroutine_handle<> cont{};

        task get_return_object() {
            return task{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        struct final_awaitable {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (auto c = h.promise().cont) c.resume();
            }
            void await_resume() noexcept {}
        };

        final_awaitable final_suspend() noexcept {
            return {};
        }

        template <class U>
        void return_value(U&& v) {
            val = std::forward<U>(v);
        }

        void unhandled_exception() {
            ep = std::current_exception();
        }
    };

    // using handle = std::coroutine_handle<promise_type>;

    std::coroutine_handle<promise_type> h{};

    explicit task(std::coroutine_handle<promise_type> hh) : h(hh) {}
    task(task&& o) noexcept : h(std::exchange(o.h, {})) {}
    task(const task&) = delete;
    task& operator=(const task&) = delete;

    ~task() {
        if (h) h.destroy();
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> awaiting) const {
        h.promise().cont = awaiting;
        h.resume();
    }

    T await_resume() const {
        if (h.promise().ep) std::rethrow_exception(h.promise().ep);
        return std::move(*h.promise().val);
    }
};

class AsyncFileIter {
public:
    AsyncFileIter(std::string path, std::size_t chunk_size)
        : chunk_size_(chunk_size) {
        state_.path = std::move(path);
    }

    task<std::optional<std::string>> next() {
        if (state_.done) {
            co_return std::nullopt;
        }

        auto data = co_await read_chunk{ state_.path, state_.off, chunk_size_ };

        if (data.empty()) {
            state_.done = true;
            co_return std::nullopt;
        }

        state_.off += static_cast<std::streamoff>(data.size());
        co_return std::optional<std::string>(std::move(data));
    }

    const std::string& path() const {
        return state_.path;
    }

private:
    FileState state_;
    std::size_t chunk_size_ = 64;
};

struct Item {
    std::string src;
    std::string seq;
    std::string data;
};

struct detached_task {
    struct promise_type {
        detached_task get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

class CompositeIter {
public:
    CompositeIter(std::vector<std::string> paths, std::size_t chunk_size) {
        for (auto& path : paths) {
            iters_.emplace_back(paths, chunk_size);
        }
        start();
    }

    task<std::optional<Item>> next() {
        co_return co_await ch_.pop();
    }

private:
    detached_task pump(std::size_t i) {
        std::size_t k = 0;
        while (1) {
            auto s = co_await iters_[i].next();
            if (!s) break;

            ch_.push(Item{
                .src = iters_[i].path(),
                .sql = k++;
                .data = std::move(*s)
            });
        }

        if (active_.fetch_sub(1) == 1) {
            ch_.close();
        }
        co_return;
    }

    void start() {
        active_.store(iters_.size());
        for (std::size_t i = 0; i < iters_.size(); i++) {
            pump(i);
        }
    }

    std::vector<AsyncFileIter> iters_;
    async_channel<Item> ch_;
    std::atomic<std::size_t> active_{0};
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <file1> [file2 ...]\n";
        return 1;
    }

    std::vector<std::string> paths;
    for (int i = 1; i < argc; i++) {
        paths.push_back(argv[i]);
    }

    CompositeIter comp(paths, 64);
    return 0;
}
