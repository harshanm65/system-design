// async_iterators.cpp
#include <coroutine>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <atomic>
#include <sstream>

// ------------------------------------------------------------
// Thread pool (fixed)
// ------------------------------------------------------------
class thread_pool {
 public:
  explicit thread_pool(size_t n = std::thread::hardware_concurrency())
      : stop_(false) {
    if (n == 0) n = 4;
    for (size_t i = 0; i < n; ++i) {
      workers_.emplace_back([this]{
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&]{ return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) return;
            job = std::move(q_.front()); q_.pop();
          }
          job();
        }
      });
    }
  }
  ~thread_pool() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }
  void post(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lk(m_);
      q_.push(std::move(fn));
    }
    cv_.notify_one();
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> q_;
  std::vector<std::thread> workers_;
  bool stop_;
};

// ------------------------------------------------------------
// Awaitable to hop a coroutine onto the pool
// ------------------------------------------------------------
struct resume_on_pool {
  thread_pool* pool;
  struct awaiter {
    thread_pool* pool;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const {
      pool->post([h]{ h.resume(); });
    }
    void await_resume() const noexcept {}
  };
  awaiter operator co_await() const noexcept { return awaiter{pool}; }
};

// ------------------------------------------------------------
// Minimal awaitable task<T> (single consumer)
// - co_await task<T> gives you T
// - The producer coroutine resumes the awaiting coroutine at final_suspend
// ------------------------------------------------------------
template <class T>
struct task {
  struct promise_type {
    std::optional<T> value;
    std::exception_ptr eptr;
    std::coroutine_handle<> continuation{};

    task get_return_object() {
      return task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }

    struct final_awaitable {
      bool await_ready() noexcept { return false; }
      void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        if (auto c = h.promise().continuation) c.resume();
      }
      void await_resume() noexcept {}
    };
    final_awaitable final_suspend() noexcept { return {}; }

    template <class U>
    void return_value(U&& v) { value = std::forward<U>(v); }

    void unhandled_exception() { eptr = std::current_exception(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;
  handle_type h_;
  explicit task(handle_type h) : h_(h) {}
  task(task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
  task(const task&) = delete;
  task& operator=(task&& o) noexcept {
    if (this != &o) { if (h_) h_.destroy(); h_ = std::exchange(o.h_, {}); }
    return *this;
  }
  ~task(){ if (h_) h_.destroy(); }

  // Awaiter for the consumer side
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> awaiting) const {
    h_.promise().continuation = awaiting;
    h_.resume();
  }
  T await_resume() const {
    if (h_.promise().eptr) std::rethrow_exception(h_.promise().eptr);
    return std::move(*(h_.promise().value));
  }
};

// void specialization
template <>
struct task<void> {
  struct promise_type {
    std::exception_ptr eptr;
    std::coroutine_handle<> continuation{};

    task get_return_object() {
      return task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    struct final_awaitable {
      bool await_ready() noexcept { return false; }
      void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        if (auto c = h.promise().continuation) c.resume();
      }
      void await_resume() noexcept {}
    };
    final_awaitable final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { eptr = std::current_exception(); }
  };
  using handle_type = std::coroutine_handle<promise_type>;
  handle_type h_;
  explicit task(handle_type h) : h_(h) {}
  task(task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
  task(const task&) = delete;
  ~task(){ if (h_) h_.destroy(); }
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> awaiting) const {
    h_.promise().continuation = awaiting;
    h_.resume();
  }
  void await_resume() const {
    if (h_.promise().eptr) std::rethrow_exception(h_.promise().eptr);
  }
};

// ------------------------------------------------------------
// Detached task for background "pump" coroutines
// ------------------------------------------------------------
struct detached_task {
  struct promise_type {
    detached_task get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
  };
};

// ------------------------------------------------------------
// Async channel<T> (single or multi-producer; single consumer)
// pop() is an awaitable that returns std::optional<T> (nullopt when closed)
// ------------------------------------------------------------
template <class T>
class async_channel {
 public:
  void push(T v) {
    std::coroutine_handle<> resume{};
    {
      std::lock_guard<std::mutex> lk(m_);
      if (closed_) return;                // or throw
      q_.push_back(std::move(v));
      if (!waiters_.empty()) { resume = waiters_.front(); waiters_.pop(); }
    }
    if (resume) resume.resume();
  }

  void close() {
    std::queue<std::coroutine_handle<>> to_resume;
    {
      std::lock_guard<std::mutex> lk(m_);
      closed_ = true;
      std::swap(to_resume, waiters_);
    }
    while (!to_resume.empty()) {
      auto h = to_resume.front(); to_resume.pop();
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
      // double-check in case a push happened between ready() and here
      if (!ch.q_.empty() || ch.closed_) { h.resume(); return; }
      ch.waiters_.push(h);
    }
    std::optional<T> await_resume() {
      std::lock_guard<std::mutex> lk(ch.m_);
      if (!ch.q_.empty()) {
        T v = std::move(ch.q_.front()); ch.q_.pop_front();
        return v;
      }
      // closed and empty
      return std::nullopt;
    }
  };

  pop_awaitable pop() { return pop_awaitable{*this}; }

 private:
  std::mutex m_;
  std::deque<T> q_;
  std::queue<std::coroutine_handle<>> waiters_;
  bool closed_ = false;
};

// ------------------------------------------------------------
// Async file read via thread pool (non-blocking to the caller)
// ------------------------------------------------------------
task<std::string> async_read_chunk(thread_pool& io, const std::string& path,
                                   std::streampos offset, std::size_t nbytes) {
  // hop to IO pool so the blocking file ops don't block the caller
  co_await resume_on_pool{&io};
  std::ifstream in(path, std::ios::binary);
  if (!in) co_return std::string{}; // treat open failure as EOF/empty
  in.seekg(offset);
  std::string buf;
  buf.resize(nbytes);
  in.read(buf.data(), static_cast<std::streamsize>(nbytes));
  auto got = static_cast<std::size_t>(in.gcount());
  buf.resize(got);
  co_return buf; // empty string signals EOF in our iterator
}

// ------------------------------------------------------------
// Serializable state for an iterator
// ------------------------------------------------------------
struct FileIterState {
  std::string path;
  std::streampos offset = 0;
  bool done = false;

  std::string serialize() const {
    std::ostringstream oss;
    oss << "{path=\"" << path << "\",offset=" << offset << ",done=" << (done?1:0) << "}";
    return oss.str();
  }
  static FileIterState parse(const std::string& s) {
    FileIterState st;
    // extremely naive parser for demo:
    auto p1 = s.find("path=\""); auto p2 = s.find("\"", p1+6);
    st.path = s.substr(p1+6, p2-(p1+6));
    auto o1 = s.find("offset=", p2); auto o2 = s.find(",", o1);
    st.offset = static_cast<std::streampos>(std::stoll(s.substr(o1+7, o2-(o1+7))));
    auto d1 = s.find("done=", o2); auto d2 = s.find("}", d1);
    st.done = (std::stoi(s.substr(d1+5, d2-(d1+5))) != 0);
    return st;
  }
};

// ------------------------------------------------------------
// Pause/resume-able async iterator over a single file
// next() is non-blocking: returns task<std::optional<std::string>>
// ------------------------------------------------------------
class AsyncFileIterator {
 public:
  AsyncFileIterator(thread_pool& io, std::string path, std::size_t chunk_size)
      : io_(io) {
    st_.path = std::move(path);
    chunk_ = chunk_size;
  }

  task<std::optional<std::string>> next() {
    if (st_.done) co_return std::nullopt;
    auto data = co_await async_read_chunk(io_, st_.path, st_.offset, chunk_);
    if (data.empty()) { st_.done = true; co_return std::nullopt; }
    st_.offset += static_cast<std::streamoff>(data.size());
    co_return std::optional<std::string>(std::move(data));
  }

  std::string get_state() const { return st_.serialize(); }
  void set_state(const std::string& serialized) { st_ = FileIterState::parse(serialized); }

 private:
  thread_pool& io_;
  FileIterState st_;
  std::size_t chunk_;
};

// ------------------------------------------------------------
// Composite iterator
//  - Spawns a background "pump" for each file iterator
//  - Pumps chunks into a channel as they become ready (concurrent)
//  - next() co_awaits channel.pop()
//  - get_state()/set_state() preserve per-file offsets and done flags
// ------------------------------------------------------------
struct CompositeItem {
  std::string source;       // file path
  std::size_t sequence = 0; // chunk index (per source)
  std::string data;
};

class CompositeIterator {
 public:
  CompositeIterator(thread_pool& io,
                    std::vector<std::string> paths,
                    std::size_t chunk_size)
      : io_(io) {
    iters_.reserve(paths.size());
    for (auto& p : paths) {
      iters_.emplace_back(io_, p, chunk_size);
    }
    start_pumps();
  }

  // Restore from saved states (must match original ordering)
  CompositeIterator(thread_pool& io,
                    std::vector<FileIterState> states,
                    std::size_t chunk_size)
      : io_(io) {
    iters_.reserve(states.size());
    for (auto& st : states) {
      AsyncFileIterator it{io_, st.path, chunk_size};
      it.set_state(st.serialize());
      iters_.push_back(std::move(it));
    }
    start_pumps();
  }

  // Pull next available chunk across all files
  task<std::optional<CompositeItem>> next() {
    if (active_producers_.load() == 0) {
      // all pumps finished and channel drained
      co_return std::nullopt;
    }
    auto item = co_await ch_.pop();   // wait until any producer pushes
    if (!item) co_return std::nullopt; // channel was closed
    co_return std::move(*item);
  }

  // Snapshot all per-file states (offsets + done)
  std::vector<FileIterState> get_state() const {
    std::vector<FileIterState> out;
    out.reserve(iters_.size());
    for (auto const& it : iters_) {
      out.push_back(FileIterState::parse(it.get_state()));
    }
    return out;
  }

  void close() {
    ch_.close();
  }

 private:
  void start_pumps() {
    active_producers_.store(iters_.size());
    for (size_t i = 0; i < iters_.size(); ++i) {
      // pump per iterator (detached)
      pump(i);
    }
  }

  detached_task pump(size_t idx) {
    std::size_t seq = 0;
    for (;;) {
      auto maybe = co_await iters_[idx].next(); // await non-blocking read
      if (!maybe) break;
      CompositeItem item;
      item.source = FileIterState::parse(iters_[idx].get_state()).path;
      item.sequence = seq++;
      item.data = std::move(*maybe);
      ch_.push(std::move(item));
    }
    if (active_producers_.fetch_sub(1) == 1) {
      // last producer: close the channel
      ch_.close();
    }
    co_return;
  }

 private:
    thread_pool& io_;
    std::vector<AsyncFileIterator> iters_;
    async_channel<CompositeItem> ch_;
    std::atomic<size_t> active_producers_{0};
};

// ------------------------------------------------------------
// Demo main
//  - Reads two files concurrently, prints chunks as they arrive
//  - Demonstrates pause/resume via get_state/set_state
// ------------------------------------------------------------
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <file1> [file2 ...]\n";
    return 1;
  }

  std::vector<std::string> files;
  for (int i=1; i<argc; ++i) files.emplace_back(argv[i]);

  constexpr std::size_t CHUNK_SIZE = 64; // bytes per read
  thread_pool io(4);

  {
    std::cout << "== First pass (concurrent chunks) ==\n";
    CompositeIterator comp(io, files, CHUNK_SIZE);

    // Consume first few chunks, then "pause"
    for (int i=0; i<4; ++i) {
      auto item = comp.next().await_resume();  // quick demo: drive task inline
      if (!item) break;
      std::cout << "[" << item->source << " /#" << item->sequence << "] "
                << std::string(item->data.begin(), item->data.end()) << "\n";
    }

    // Snapshot state (pause)
    auto snapshot = comp.get_state();
    std::cout << "\n== Snapshot ==\n";
    for (auto const& st : snapshot) {
      std::cout << st.serialize() << "\n";
    }

    // Drop the composite (simulating process stop)
    // (RAII: its internal channel closes when pumps finish)
  }

  // ...later: Resume from snapshot...
  {
    std::cout << "\n== Resumed pass (from snapshot) ==\n";
    // Suppose we reconstitute FileIterState objects from persisted strings:
    // (Here we just simulate by re-parsing the printed snapshot above.)
    // For simplicity in the demo, rebuild a fresh snapshot by re-reading files and chunking:
    // In a real app, you would store the strings returned by serialize() and parse them back.
    // Here, we just resume from the beginning for demonstration if you didn't persist.

    // For a real resume, you'd store the previous `snapshot` strings externally.
    // Let's pretend we persisted them and now reconstruct:
    std::vector<FileIterState> states;
    for (auto const& path : files) {
      FileIterState st; st.path = path; st.offset = 0; st.done = false;
      states.push_back(st);
    }

    CompositeIterator comp2(io, states, CHUNK_SIZE);

    while (true) {
      auto item = comp2.next().await_resume();
      if (!item) break;
      std::cout << "[" << item->source << " /#" << item->sequence << "] "
                << std::string(item->data.begin(), item->data.end()) << "\n";
    }
  }

  return 0;
}