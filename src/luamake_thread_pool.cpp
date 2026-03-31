#include "luamake_thread_pool.hpp"

#include "luamake_builtins.hpp"

namespace luamake {
CompilationPool threads = CompilationPool();

CompilationPool::~CompilationPool() noexcept {
  for (auto &thread : workers) {
    if (thread.joinable()) // ?
      thread.join();
  }
}

auto CompilationPool::init(size_t num_threads) noexcept -> void {
  workers.reserve(num_threads);
}

auto CompilationPool::deinit() noexcept -> void {
  for (auto &thread : workers) {
    if (thread.joinable()) // ?
      thread.join();
  }
}

auto CompilationPool::busy() noexcept -> bool {
  // std::this_thread::sleep_for(std::chrono::nanoseconds(100));
  auto pool_busy = true;
  {
    auto lock = std::unique_lock(task_mtx);
    pool_busy = !tasks.empty();
  }
  return pool_busy;
}
} // namespace luamake
