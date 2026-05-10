#include "luamake_thread_pool.hpp"

#include "common.hpp"
#include "luamake_builtins.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include <iostream>

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

// NOTE: i finally got this working as a vectorized add_task, but it seems
// slower than before my only thought is that because we do a notify_all,
// instead of a notify_one, all of the threads are fighting for the task_mtx,
// while before with the slight delay there was enough time for the task_mtx to
// be unlocked(?), either way it seems slower but i was about to pull out my
// hair dealing with race conditions so i'll leave it here until somebody else
// comes along and makes it better
auto CompilationPool::add_dep_tree_tasks(builtins::ModIndex const idx) noexcept
    -> void {
  auto const parent_path = builtins::mods.get_module_path(idx).parent_path();
  auto const &mod = builtins::mods.module_at(idx);
  auto const include_path = mod.format_includes();

  // auto lock = std::unique_lock(builtins::mods.mtxs[idx.mods]);
  // builtins::mods.remaining_files[idx.mods] = num_files_to_compile;

  // so ig we do this to show intent that we will want to aquire this mutex, but
  // that we won't block because we don't need it now(?)
  auto mod_lock =
      std::unique_lock(builtins::mods.mtxs[idx.mods], std::defer_lock);
  {
    auto task_lock = std::unique_lock(task_mtx);
    auto num_remaining_files = size_t{};
    for (auto i = size_t{}; i < mod.tree.num_files; ++i) {
      auto const fname = std::filesystem::relative(
          parent_path / mod.tree.get_path(i), std::filesystem::current_path());

      if (builtins::Module::DepTree::determine_file_type(fname.extension()) !=
          builtins::Module::DepTree::SourceFile_t::IMPL) {
        continue;
      }

      {
        // auto lock = std::unique_lock(builtins::mods.mtxs[idx.mods]);
        // builtins::mods.remaining_files[idx.mods]++;
        ++num_remaining_files;
      }

      tasks.push([idx, include_path, compiler = mod.compiler,
                  install_dir = mod.install_dir, name = mod.name,
                  path = fname]() -> void {
        auto const invoked_command =
            std::format("{} {} -c {} -o {}/{}.o/{}.o", compiler, include_path,
                        path.c_str(), install_dir, name, path.stem().c_str());
        // idk i tried using std::cout, but there was an error :)
        if (builtins::cl_options.verbose) {
          fprintf(stdout, "[%s]" NL, invoked_command.c_str());
        } else {
          fprintf(stdout, "Building [%s]" NL, path.c_str());
        }
        if (OS_CALL(invoked_command.c_str()) == 0) {
          builtins::mods.add_compiled_file(idx, path.stem().string());
        } else {
          builtins::mods.set_state_at(idx,
                                      builtins::LakeModules::ModState::error);
        }
      });
    }
    mod_lock.lock();
    builtins::mods.remaining_files[idx.mods] = num_remaining_files;
  }
  waiting.notify_all();
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
      waiting.wait(lock,
                   [this]() { return !tasks.empty() || should_terminate; });
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
