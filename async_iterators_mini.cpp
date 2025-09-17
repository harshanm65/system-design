// async_iter_mvp.cpp  (C++20)
// Minimal async, pause/resume-able composite iterator over multiple files.
#include <coroutine>
#include <fstream>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <deque>
#include <optional>
#include <iostream>
#include <atomic>
#include <future>
#include <sstream>

//-----------------------------
// read_chunk_awaitable
//-----------------------------
struct read_chunk_awaitable {
  std::string path;
  std::streampos offset;
  std::size_t nbytes;
  std::shared_ptr<std::string> out = std::make_shared<std::string>();

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    // Do blocking I/O on a short-lived thread; resume coroutine when done.
    std::thread([=]{
      std::ifstream in(path, std::ios::binary);
      if (!in) { *out = {}; h.resume(); return; }
      in.seekg(offset);
      std::string buf; buf.resize(nbytes);
      in.read(buf.data(), static_cast<std::streamsize>(nbytes));
      buf.resize(static_cast<std::size_t>(in.gcount()));
      *out = std::move(buf);
      h.resume();
    }).detach();
  }
  std::string await_resume() const noexcept { return std::move(*out); }
};

//-----------------------------
// task<T>  (single consumer)
//-----------------------------
template<class T>
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
    template<class U> void return_value(U&& v) { value = std::forward<U>(v); }
    void unhandled_exception() { eptr = std::current_exception(); }
  };

  using handle = std::coroutine_handle<promise_type>;
  handle h{};
  explicit task(handle h_) : h(h_) {}
  task(task&& o) noexcept : h(std::exchange(o.h, {})) {}
  task(const task&) = delete;
  task& operator=(task&& o) noexcept { if(this!=&o){ if(h) h.destroy(); h=std::exchange(o.h,{});} return *this; }
  ~task(){ if(h) h.destroy(); }

  // Awaiter (for coroutines)
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> awaiting) const {
    h.promise().continuation = awaiting;
    h.resume();
  }
  T await_resume() const {
    if (h.promise().eptr) std::rethrow_exception(h.promise().eptr);
    return std::move(*h.promise().value);
  }
};

// void spec not needed in this MVP; we only use task<T>.

//-----------------------------
// detached_task  (fire-and-forget)
//-----------------------------
struct detached_task {
  struct promise_type {
    detached_task get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend()   noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
  };
};

//-----------------------------
// async_channel<T>  (fan-in buffer)
// pop() returns awaitable: std::optional<T> (nullopt when closed)
//-----------------------------
template<class T>
class async_channel {
  std::mutex m;
  std::deque<T> q;
  std::queue<std::coroutine_handle<>> waiters;
  bool closed = false;

public:
  void push(T v) {
    std::coroutine_handle<> to_resume{};
    {
      std::lock_guard<std::mutex> lk(m);
      if (closed) return;
      q.push_back(std::move(v));
      if (!waiters.empty()) { to_resume = waiters.front(); waiters.pop(); }
    }
    if (to_resume) to_resume.resume();
  }

  void close() {
    std::queue<std::coroutine_handle<>> resume_all;
    {
      std::lock_guard<std::mutex> lk(m);
      closed = true;
      std::swap(resume_all, waiters);
    }
    while (!resume_all.empty()) { auto h = resume_all.front(); resume_all.pop(); h.resume(); }
  }

  struct pop_awaitable {
    async_channel& ch;
    bool await_ready() {
      std::lock_guard<std::mutex> lk(ch.m);
      return (!ch.q.empty() || ch.closed);
    }
    void await_suspend(std::coroutine_handle<> h) {
      std::lock_guard<std::mutex> lk(ch.m);
      if (!ch.q.empty() || ch.closed) { h.resume(); return; }
      ch.waiters.push(h);
    }
    std::optional<T> await_resume() {
      std::lock_guard<std::mutex> lk(ch.m);
      if (!ch.q.empty()) { T v = std::move(ch.q.front()); ch.q.pop_front(); return v; }
      return std::nullopt; // closed & empty
    }
  };

  pop_awaitable pop() { return pop_awaitable{*this}; }
};

//-----------------------------
// Serializable per-file state
//-----------------------------
struct FileIterState {
  std::string path;
  std::streampos offset = 0;
  bool done = false;

  std::string serialize() const {
    std::ostringstream oss;
    oss << "{path=\"" << path << "\",offset=" << static_cast<long long>(offset)
        << ",done=" << (done?1:0) << "}";
    return oss.str();
  }
  static FileIterState parse(const std::string& s) {
    FileIterState st;
    auto p1 = s.find("path=\""), p2 = s.find("\"", p1+6);
    st.path = s.substr(p1+6, p2-(p1+6));
    auto o1 = s.find("offset=", p2), o2 = s.find(",", o1);
    st.offset = static_cast<std::streampos>(std::stoll(s.substr(o1+7, o2-(o1+7))));
    auto d1 = s.find("done=", o2), d2 = s.find("}", d1);
    st.done = (std::stoi(s.substr(d1+5, d2-(d1+5))) != 0);
    return st;
  }
};

//-----------------------------
// AsyncFileIterator: non-blocking next()
//-----------------------------
class AsyncFileIterator {
  FileIterState st;
  std::size_t chunk = 64;

public:
  AsyncFileIterator(std::string path, std::size_t chunk_size)
    { st.path = std::move(path); chunk = chunk_size; }

  task<std::optional<std::string>> next() {
    if (st.done) co_return std::nullopt;
    auto data = co_await read_chunk_awaitable{st.path, st.offset, chunk};
    if (data.empty()) { st.done = true; co_return std::nullopt; }
    st.offset += static_cast<std::streamoff>(data.size());
    co_return std::optional<std::string>(std::move(data));
  }

  std::string get_state() const { return st.serialize(); }
  void set_state(const std::string& s) { st = FileIterState::parse(s); }
  const std::string& path() const { return st.path; }
};

//-----------------------------
// Composite iterator
//-----------------------------
struct CompositeItem {
  std::string source;
  std::size_t sequence = 0;
  std::string data;
};

class CompositeIterator {
  std::vector<AsyncFileIterator> iters;
  async_channel<CompositeItem> ch;
  std::atomic<size_t> active{0};

  detached_task pump(size_t i) {
    std::size_t seq = 0;
    for (;;) {
      auto maybe = co_await iters[i].next();
      if (!maybe) break;
      ch.push(CompositeItem{ iters[i].path(), seq++, std::move(*maybe) });
    }
    if (active.fetch_sub(1) == 1) ch.close();
    co_return;
  }

  void start() {
    active.store(iters.size());
    for (size_t i=0;i<iters.size();++i) pump(i); // detached
  }

public:
  CompositeIterator(std::vector<std::string> paths, std::size_t chunk_size) {
    iters.reserve(paths.size());
    for (auto& p : paths) iters.emplace_back(p, chunk_size);
    start();
  }
  CompositeIterator(std::vector<FileIterState> states, std::size_t chunk_size) {
    iters.reserve(states.size());
    for (auto& st : states) {
      AsyncFileIterator it{st.path, chunk_size};
      it.set_state(st.serialize());
      iters.push_back(std::move(it));
    }
    start();
  }

  task<std::optional<CompositeItem>> next() { co_return co_await ch.pop(); }
  std::vector<FileIterState> get_state() const {
    std::vector<FileIterState> out; out.reserve(iters.size());
    for (auto const& it : iters) out.push_back(FileIterState::parse(it.get_state()));
    return out;
  }
};

//-----------------------------
// Demo consumer coroutine
//-----------------------------
detached_task consume_all(CompositeIterator& comp, std::promise<void>& done) {
  while (true) {
    auto item = co_await comp.next();
    if (!item) break;
    std::cout << "[" << item->source << " /#" << item->sequence << "] "
              << std::string(item->data.begin(), item->data.end()) << "\n";
  }
  done.set_value();
  co_return;
}

//-----------------------------
// main
//-----------------------------
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <file1> [file2 ...]\n";
    return 1;
  }
  std::vector<std::string> paths;
  for (int i=1;i<argc;++i) paths.push_back(argv[i]);

  constexpr std::size_t CHUNK = 64;

  // First pass: read concurrently and print a few chunks, snapshot state.
  {
    CompositeIterator comp(paths, CHUNK);
    std::promise<void> done; auto fut = done.get_future();
    consume_all(comp, done);        // detached consumer
    fut.wait();                     // wait until channel closes (all pumps done)
  }

  // Example of resume from saved state (here we just start fresh for brevity):
  // In real use, persist comp.get_state() strings and rebuild with them.
  {
    std::vector<FileIterState> states;
    for (auto& p : paths) states.push_back(FileIterState{p, 0, false});
    CompositeIterator comp2(states, CHUNK);
    std::promise<void> done; auto fut = done.get_future();
    consume_all(comp2, done);
    fut.wait();
  }
  return 0;
}
