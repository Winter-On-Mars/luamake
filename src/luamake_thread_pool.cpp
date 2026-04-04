#include "luamake_thread_pool.hpp"

#include "common.hpp"
#include "luamake_builtins.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

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
  for (auto i = size_t{}; i < num_threads; ++i) {
    workers.push_back(std::thread([this]() { _loop(); }));
  }
}

auto CompilationPool::deinit() noexcept -> void {
  // idk if this is actually what we want to do, we should probably make sure
  // there's no tasks left in the queue
  auto lock = std::unique_lock(task_mtx);
  while (!tasks.empty()) {
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::nanoseconds{1000});
    lock.lock();
  }
  should_terminate = true;
  lock.unlock();
  waiting.notify_all();
  for (auto &thread : workers) {
    if (thread.joinable()) // ?
      thread.join();
  }
  workers.clear();
}

// TODO: update this function to just take a control of the mutex, then push
// back all of the functions like a vectorized version of add_task
auto CompilationPool::add_dep_tree_tasks(
    builtins::ModIndex const idx,
    builtins::Module::DepTree const &tree) noexcept -> void {
  expr_dbg(idx);
  auto const &mod = builtins::mods.module_at(idx);
  auto const include_path = mod.format_includes();
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    auto const fname = tree.get_path(i);

    if (builtins::Module::DepTree::determine_file_type(fname.extension()) !=
        builtins::Module::DepTree::SourceFile_t::IMPL) {
      fprintf(stdout, "file [%s] is not an impl, skipping\n", fname.c_str());
      continue;
    }

    {
      // idk if we actually have to aquire the lock because they're atomic(?)
      auto lock = std::unique_lock(builtins::mods.mtxs[idx.idx]);
      builtins::mods.remaining_files[idx.idx]++;
    }

    add_task([idx, include_path, compiler = mod.compiler,
              install_dir = mod.install_dir, name = mod.name,
              path = fname]() -> void {
      fprintf(stdout, "\tworking with path [%s]\n", path.c_str());
      auto const invoked_command =
          std::format("{} {} -c {} -o {}/{}.o/{}.o", compiler, include_path,
                      path.c_str(), install_dir, name, path.stem().c_str());
      // idk i tried using std::cout, but there was an error :)
      fprintf(stdout, "[%s]\n", invoked_command.c_str());
      if (OS_CALL(invoked_command.c_str()) == 0) {
        builtins::mods.add_compiled_file(idx, path.stem().string());
      } else {
        builtins::mods.set_state_at(idx,
                                    builtins::LakeModules::ModState::error);
      }
    });
  }
}

auto CompilationPool::add_task(std::function<void()> &&func) noexcept -> void {
  {
    auto lock = std::unique_lock(task_mtx);
    tasks.push(std::move(func));
  }
  waiting.notify_one();
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

auto CompilationPool::_loop() noexcept -> void {
  while (true) {
    auto job = std::function<void()>();
    {
      auto lock = std::unique_lock(task_mtx);
      waiting.wait(lock, [this] { return !tasks.empty() || should_terminate; });
      if (should_terminate) {
        return;
      }
      job = tasks.front();
      tasks.pop();
    }
    job();
  }
}
} // namespace luamake
