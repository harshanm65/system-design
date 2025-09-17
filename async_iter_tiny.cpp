// async_iter_tiny.cpp  (C++20)
// Minimal async, pause/resume-able composite iterator (cleanly formatted).

#include <atomic>
#include <coroutine>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------------
// Awaitable: read a file chunk on a short-lived thread, then resume.
// -----------------------------------------------------------------------------
struct read_chunk {
  std::string                       path;
  std::streampos                    off;
  std::size_t                       n;
  std::shared_ptr<std::string>      out = std::make_shared<std::string>();

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

// -----------------------------------------------------------------------------
// Minimal task<T> (single-consumer) to bridge coroutines.
// -----------------------------------------------------------------------------
template <class T>
struct task {
  struct promise_type {
    std::optional<T>             val;
    std::exception_ptr           ep;
    std::coroutine_handle<>      cont{};

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

  using handle = std::coroutine_handle<promise_type>;

  handle h{};

  explicit task(handle hh) : h(hh) {}
  task(task&& o) noexcept : h(std::exchange(o.h, {})) {}
  task(const task&)            = delete;
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

// -----------------------------------------------------------------------------
// Tiny async_channel<T> (single consumer): push/pop with coroutine-aware wait.
// -----------------------------------------------------------------------------
template <class T>
class async_channel {
public:
  void push(T v) {
    std::coroutine_handle<> to_resume{};

    {
      std::lock_guard<std::mutex> lk(m_);
      if (closed_) return;
      q_.push_back(std::move(v));
      if (!waiters_.empty()) {
        to_resume = waiters_.front();
        waiters_.pop();
      }
    }

    if (to_resume) to_resume.resume();
  }

  void close() {
    std::queue<std::coroutine_handle<>> to_wake;

    {
      std::lock_guard<std::mutex> lk(m_);
      closed_ = true;
      std::swap(to_wake, waiters_);
    }

    while (!to_wake.empty()) {
      auto h = to_wake.front();
      to_wake.pop();
      h.resume();
    }
  }

  struct pop_awaitable {
    async_channel& ch;

    bool await_ready() {
      std::lock_guard<std::mutex> lk(ch.m_);
      return (!ch.q_.empty() || ch.closed_);
    }

    void await_suspend(std::coroutine_handle<> h) {
      std::lock_guard<std::mutex> lk(ch.m_);
      if (!ch.q_.empty() || ch.closed_) {
        h.resume();
      } else {
        ch.waiters_.push(h);
      }
    }

    std::optional<T> await_resume() {
      std::lock_guard<std::mutex> lk(ch.m_);
      if (!ch.q_.empty()) {
        T v = std::move(ch.q_.front());
        ch.q_.pop_front();
        return v;
      }
      return std::nullopt; // closed & empty
    }
  };

  pop_awaitable pop() { return pop_awaitable{ *this }; }

private:
  std::mutex                               m_;
  std::deque<T>                            q_;
  std::queue<std::coroutine_handle<>>      waiters_;
  bool                                     closed_ = false;
};

// -----------------------------------------------------------------------------
// Per-file iterator state (simple string serialize/parse).
// -----------------------------------------------------------------------------
struct FileState {
  std::string    path;
  std::streampos off  = 0;
  bool           done = false;

  std::string serialize() const {
    return path + "|" +
           std::to_string(static_cast<long long>(off)) + "|" +
           (done ? "1" : "0");
  }

  void parse(const std::string& s) {
    auto a = s.find('|');
    auto b = s.find('|', a + 1);

    path = s.substr(0, a);

    auto off_str = s.substr(a + 1, b - a - 1);
    off = static_cast<std::streampos>(std::stoll(off_str));

    auto done_str = s.substr(b + 1);
    done = (done_str == "1");
  }
};

// -----------------------------------------------------------------------------
// Async per-file iterator (non-blocking next()).
// -----------------------------------------------------------------------------
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

  std::string get_state() const {
    return state_.serialize();
  }

  void set_state(const std::string& s) {
    state_.parse(s);
  }

  const std::string& path() const {
    return state_.path;
  }

private:
  FileState     state_;
  std::size_t   chunk_size_ = 64;
};

// -----------------------------------------------------------------------------
// Composite iterator: concurrent pumps + fan-in channel.
// -----------------------------------------------------------------------------
struct Item {
  std::string  src;
  std::size_t  seq;
  std::string  data;
};

struct detached_task {
  struct promise_type {
    detached_task get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend()   noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
  };
};

class CompositeIter {
public:
  CompositeIter(std::vector<std::string> paths, std::size_t chunk) {
    for (auto& p : paths) {
      iters_.emplace_back(p, chunk);
    }
    start();
  }

  task<std::optional<Item>> next() {
    co_return co_await ch_.pop();
  }

  std::vector<std::string> get_state() const {
    std::vector<std::string> v;
    v.reserve(iters_.size());
    for (auto& it : iters_) {
      v.push_back(it.get_state());
    }
    return v;
  }

private:
  detached_task pump(std::size_t i) {
    std::size_t k = 0;
    for (;;) {
      auto s = co_await iters_[i].next();
      if (!s) break;

      ch_.push(Item{
        .src  = iters_[i].path(),
        .seq  = k++,
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
    for (std::size_t i = 0; i < iters_.size(); ++i) {
      pump(i); // detached
    }
  }

  std::vector<AsyncFileIter>  iters_;
  async_channel<Item>         ch_;
  std::atomic<std::size_t>    active_{0};
};

// -----------------------------------------------------------------------------
// Simple consumer driver to keep main alive while coroutines run.
// -----------------------------------------------------------------------------
struct done_signal {
  std::promise<void> p;

  std::future<void> get() {
    return p.get_future();
  }

  void set() {
    p.set_value();
  }
};

detached_task consume(CompositeIter& c, done_signal& d) {
  while (true) {
    auto x = co_await c.next();
    if (!x) break;

    std::cout
      << "[" << x->src << " #" << x->seq << "] "
      << x->data
      << "\n";
  }
  d.set();
  co_return;
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <file1> [file2 ...]\n";
    return 1;
  }

  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i) {
    paths.push_back(argv[i]);
  }

  CompositeIter comp(paths, 64);

  done_signal done;
  std::future<void> fut = done.get();

  consume(comp, done);   // detached consumer coroutine
  fut.wait();            // wait until channel closes (all pumps finished)

  return 0;
}
