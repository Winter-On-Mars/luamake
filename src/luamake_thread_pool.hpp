#ifndef __LUAMAKE_THREAD_POOL_HPP
#define __LUAMAKE_THREAD_POOL_HPP

#include "luamake_builtins.hpp"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <span>
#include <string_view>
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

  auto add_compile_tasks(std::span<std::string_view const> const,
                         builtins::ModIndex const) noexcept -> void;

  template <class T> auto add_task(T &&func) noexcept -> void {
    {
      auto lock = std::unique_lock(task_mtx);
      tasks.push(std::move(func));
    }
    waiting.notify_one();
  }

  // there are probably better ways to manage these threads, idk return an raii
  // object that does this(?), also error handling :)
  auto reserve_threads(size_t const num_threads) noexcept -> void;
  auto give_back_threads(size_t const num_threads) noexcept -> void;

private:
  auto busy() noexcept -> bool;
  auto should_terminate() const noexcept -> bool;
  auto set_terminate() noexcept -> void;

  auto num_threads() const noexcept -> size_t;

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
  // idk i'm using the top bit to represent, we could probably switch this to
  // the top byte, making it a bit faster to load, because then we only need to
  // load a byte, not a bit, also 32bit support (idk if that's something i've
  // tried to maintain but this currently breaks things)
  static auto constexpr terminate_bit = size_t{1} << 63;
  size_t available_threads = size_t{};
};
extern CompilationPool threads;
} // namespace luamake

#endif // !__LUAMAKE_THREAD_POOL_HPP
