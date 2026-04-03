#ifndef __LUAMAKE_THREAD_POOL_HPP
#define __LUAMAKE_THREAD_POOL_HPP

#include "luamake_builtins.hpp"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace luamake {
// sort of a thread pool like structure that is just for compiling
// TODO: update this to take advantage of the current layout for DepTree
// i.e. relying on DepTree.types to determine what to compile
// TODO: rewrite the system so that this can run in the background while we
// build the dep tree for other modules, and just queue jobs into this as needed
struct CompilationPool final {
  CompilationPool() = default;

  // init the thread pool
  CompilationPool(size_t num_threads) noexcept = delete;

  ~CompilationPool() noexcept;

  auto init(size_t num_threads) noexcept -> void;
  auto deinit() noexcept -> void;

  auto add_dep_tree_tasks(lua_Integer const,
                          luamake::builtins::Module::DepTree const &) noexcept
      -> void;

  // TODO: try and template this, it might give better source code
  auto add_task(std::function<void()> &&) noexcept -> void;

private:
  auto busy() noexcept -> bool;

  CompilationPool(CompilationPool &&) = delete;
  CompilationPool &operator=(CompilationPool &&) = delete;
  CompilationPool(CompilationPool const &) = delete;
  CompilationPool &operator=(CompilationPool const &) = delete;

  std::vector<std::thread> workers;
  std::vector<std::function<void()>> tasks;
  std::condition_variable waiting;
  std::mutex task_mtx;
};
extern CompilationPool threads;
} // namespace luamake

#endif // !__LUAMAKE_THREAD_POOL_HPP
