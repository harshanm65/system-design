// crawler_bfs.cpp  (C++20)
// Multithreaded BFS web crawler (engine + mock fetcher).
// Replace MockFetcher with a real HTTP/HTML fetcher for live crawling.

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ------------------------------- URL helpers -------------------------------
std::string get_domain(const std::string& url) {
  // crude but effective: extract host part
  static const std::regex re(R"(^(?:https?://)([^/]+))", std::regex::icase);
  std::smatch m;
  if (std::regex_search(url, m, re)) return m[1].str();
  return {};
}

std::string strip_fragment_query(const std::string& url) {
  auto hash = url.find('#');
  auto qm   = url.find('?');
  auto cut  = std::min(hash == std::string::npos ? url.size() : hash,
                       qm   == std::string::npos ? url.size() : qm);
  return url.substr(0, cut);
}

std::string normalize_url(const std::string& url) {
  // keep it simple: strip fragment/query, strip trailing slash (except root)
  std::string u = strip_fragment_query(url);
  if (u.size() > 1 && u.back() == '/') u.pop_back();
  return u;
}

// ------------------------------- Fetcher API --------------------------------
struct Fetcher {
  virtual ~Fetcher() = default;
  virtual std::vector<std::string> fetch_links(const std::string& url) = 0;
};

// A quick in-memory graph to demo BFS + multithreading without network deps.
struct MockFetcher : Fetcher {
  // adjacency list: URL -> outgoing links
  std::unordered_map<std::string, std::vector<std::string>> graph;

  explicit MockFetcher(std::string domain) {
    // Build a tiny same-domain link graph
    auto base = "https://" + domain;
    graph[base]                  = { base + "/a", base + "/b" };
    graph[base + "/a"]           = { base + "/a1", base + "/a2", base + "/b" };
    graph[base + "/b"]           = { base + "/b1", base + "/a2" };
    graph[base + "/a1"]          = {};
    graph[base + "/a2"]          = { base + "/a2x" };
    graph[base + "/a2x"]         = {};
    graph[base + "/b1"]          = {};
  }

  std::vector<std::string> fetch_links(const std::string& url) override {
    // Simulate network delay and return neighbors
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto it = graph.find(url);
    if (it == graph.end()) return {};
    return it->second;
  }
};

// --------------------------- Crawler (BFS, MT) ------------------------------
class WebCrawler {
public:
  WebCrawler(Fetcher& fetcher, std::size_t max_workers = std::thread::hardware_concurrency())
    : fetcher_(fetcher),
      max_workers_(max_workers ? max_workers : 4),
      pending_(0),
      stop_(false) {}

  // Returns all visited URLs (same domain) in BFS order (best effort under MT).
  std::vector<std::string> crawl(const std::string& start_url) {
    const std::string start = normalize_url(start_url);
    const std::string domain = get_domain(start);
    if (domain.empty()) return {};

    // Init shared structures
    {
      std::lock_guard<std::mutex> lk(visited_m_);
      visited_.clear();
      visited_.insert(start);
    }
    {
      std::lock_guard<std::mutex> lk(q_m_);
      q_.push(start);
      pending_.store(1, std::memory_order_relaxed);
      stop_ = false;
    }

    // Launch worker threads
    std::vector<std::thread> workers;
    workers.reserve(max_workers_);
    for (std::size_t i = 0; i < max_workers_; ++i) {
      workers.emplace_back([this, domain] { worker_loop(domain); });
    }

    // Wait for all work to finish
    {
      std::unique_lock<std::mutex> lk(q_m_);
      done_cv_.wait(lk, [this]{ return stop_ && q_.empty(); });
    }

    // Join workers
    for (auto& t : workers) t.join();

    // Gather results (visited set) into a vector
    std::vector<std::string> out;
    out.reserve(visited_.size());
    {
      std::lock_guard<std::mutex> lk(visited_m_);
      for (auto& u : visited_) out.push_back(u);
    }
    return out;
  }

private:
  void worker_loop(const std::string& domain) {
    while (true) {
      std::string url;

      // --- Get work ---
      {
        std::unique_lock<std::mutex> lk(q_m_);
        q_cv_.wait(lk, [this]{
          // wake if there is work OR we are stopping
          return !q_.empty() || stop_;
        });

        if (stop_ && q_.empty()) return; // nothing left, exit

        url = std::move(q_.front());
        q_.pop();
      }

      // --- Process: fetch links ---
      auto links = fetcher_.fetch_links(url);

      // --- Enqueue new URLs (same domain, not yet visited) ---
      for (auto& raw : links) {
        const std::string u = normalize_url(raw);
        if (get_domain(u) != domain) continue;

        bool inserted = false;
        {
          std::lock_guard<std::mutex> lk(visited_m_);
          inserted = visited_.insert(u).second;
        }
        if (inserted) {
          pending_.fetch_add(1, std::memory_order_relaxed);
          {
            std::lock_guard<std::mutex> lk(q_m_);
            q_.push(u);
          }
          q_cv_.notify_one();
        }
      }

      // --- This task done ---
      if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // last unit of work finished → signal stop
        {
          std::lock_guard<std::mutex> lk(q_m_);
          stop_ = true;
        }
        q_cv_.notify_all();
        done_cv_.notify_all();
        return;
      }
    }
  }

private:
  Fetcher&                        fetcher_;
  std::size_t                     max_workers_;

  // Shared FIFO for BFS
  std::queue<std::string>         q_;
  std::mutex                      q_m_;
  std::condition_variable         q_cv_;

  // Visited set
  std::unordered_set<std::string> visited_;
  std::mutex                      visited_m_;

  // Work tracking and shutdown
  std::atomic<std::size_t>        pending_;  // number of queued/active URLs
  bool                            stop_;
  std::condition_variable         done_cv_;
};

// ------------------------------- Demo main ----------------------------------
int main() {
  const std::string start = "https://example.com";
  MockFetcher fetcher(get_domain(start));
  WebCrawler crawler(fetcher, /*max_workers=*/4);

  auto visited = crawler.crawl(start);

  std::cout << "Visited " << visited.size() << " URLs:\n";
  for (auto& u : visited) std::cout << "  " << u << "\n";
  return 0;
}