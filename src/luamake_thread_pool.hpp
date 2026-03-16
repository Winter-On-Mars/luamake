#ifndef __LUAMAKE_THREAD_POOL_HPP
#define __LUAMAKE_THREAD_POOL_HPP

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace luamake {
struct ThreadResult final {
  enum class Status {
    DONE,
    FAILED,
    QUEUED,
    UNQUEUED,
  };
  Status status;
  std::byte buffer[1024];
};
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

  // TODO: have this return the thread used
  auto add_task(std::string_view const) noexcept -> ThreadResult;

private:
  auto run() -> void;
  auto busy() noexcept -> bool;
  auto get() -> std::string;

  CompilationPool(CompilationPool &&) = delete;
  CompilationPool &operator=(CompilationPool &&) = delete;
  CompilationPool(CompilationPool const &) = delete;
  CompilationPool &operator=(CompilationPool const &) = delete;

  auto _thread_loop() noexcept -> void;

  std::vector<std::thread> workers;
  std::vector<std::filesystem::path> remaining_tasks;
  std::mutex task_mtx;

  std::string result = std::string();
  std::mutex result_mtx;
};
extern CompilationPool threads;
} // namespace luamake

#endif // !__LUAMAKE_THREAD_POOL_HPP
