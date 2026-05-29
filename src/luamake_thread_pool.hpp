#ifndef __LUAMAKE_THREAD_POOL_HPP
#define __LUAMAKE_THREAD_POOL_HPP

#include "luamake_builtins.hpp"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <span>
#include <thread>
#include <vector>

namespace luamake {
// sort of a thread pool like structure that is just for compiling
struct CompilationPool final {
  CompilationPool() = default;

  // init the thread pool
  CompilationPool(size_t num_threads) noexcept = delete;

  ~CompilationPool() noexcept;

  auto init(size_t num_threads) noexcept -> void;
  auto deinit() noexcept -> void;

  auto add_dep_tree_tasks(std::span<std::filesystem::path const> const,
                          builtins::ModIndex const) noexcept -> void;

  template <class T> auto add_task(T &&func) noexcept -> void {
    {
      auto lock = std::unique_lock(task_mtx);
      tasks.push(std::move(func));
    }
    waiting.notify_one();
  }

private:
  auto busy() noexcept -> bool;

  auto _loop() noexcept -> void;

  CompilationPool(CompilationPool &&) = delete;
  CompilationPool &operator=(CompilationPool &&) = delete;
  CompilationPool(CompilationPool const &) = delete;
  CompilationPool &operator=(CompilationPool const &) = delete;

  std::vector<std::thread> workers;
  // if the queue becomes too slow, figure out how to switch to the vector
  // std::vector<std::function<void()>> tasks;
  std::queue<std::function<void()>> tasks;
  std::condition_variable waiting;
  std::mutex task_mtx;
  bool should_terminate = false;
};
extern CompilationPool threads;
} // namespace luamake

#endif // !__LUAMAKE_THREAD_POOL_HPP
