#include "Utility.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

const int WORKER_CNT = 1;

namespace util {

namespace {

struct Worker {
  Worker()
      : done_(false), thread_([=] {
          while (!done_) {
            std::unique_lock<std::mutex> lock(mutex_);
            nonempty_.wait(lock, [=] { return !tasks_.empty() || done_; });
            while (!tasks_.empty()) {
              {
                auto task = tasks_.back();
                tasks_.pop_back();
                lock.unlock();
                task();
              }
              lock.lock();
            }
          }
        }) {}

  ~Worker() {
    done_ = true;
    nonempty_.notify_one();
    thread_.join();
  }

  void add(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(task);
    }
    if (!tasks_.empty()) nonempty_.notify_one();
  }

  int task_cnt() {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
  }

  std::mutex mutex_;
  std::vector<std::function<void()>> tasks_;
  std::condition_variable nonempty_;
  std::atomic_bool done_;
  std::thread thread_;
} worker[WORKER_CNT];

}  // namespace

void enqueue(std::function<void()> f) {
  auto best = &worker[0];
  for (size_t i = 1; i < WORKER_CNT; i++)
    if (best->task_cnt() > worker[i].task_cnt()) best = &worker[i];
  best->add(f);
}

}  // namespace util
