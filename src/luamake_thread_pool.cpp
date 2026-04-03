#include "luamake_thread_pool.hpp"

#include "luamake_builtins.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>

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

// TODO: update this function to just take a control of the mutex, then push
// back all of the functions like a vectorized version of add_task
auto CompilationPool::add_dep_tree_tasks(
    lua_Integer const mod_idx, builtins::Module::DepTree const &tree) noexcept
    -> void {
  fprintf(stdout, "%s\n", __PRETTY_FUNCTION__);
  auto const &mod = builtins::mods.module_at(mod_idx);
  auto const include_path = mod.format_includes();
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    auto const fname = tree.get_path(i);
    fprintf(stdout, "fname = [%s]\n", fname.c_str());
    // TODO: there's some error causing nothing to happen because we're skipping
    // over cpp and c files :), idk fix it somehow
    if (builtins::Module::DepTree::determine_file_type(fname.extension()) !=
        builtins::Module::DepTree::SourceFile_t::IMPL) {
      fprintf(stdout, "file [%s] is not an impl, skipping\n", fname.c_str());
      continue;
    }
    add_task([mod_idx, include_path, compiler = mod.compiler,
              install_dir = mod.install_dir, name = mod.name,
              path = fname]() -> void {
      fprintf(stdout, "\tworking with path [%s]\n", path.c_str());
      auto const invoked_command =
          std::format("{} {} -c {} -o {}/{}.o/{}.o", compiler, include_path,
                      path.c_str(), install_dir, name, path.stem().c_str());
      // idk i tried using std::cout, but there was an error :)
      fprintf(stdout, "[%s]\n", invoked_command.c_str());
      if (OS_CALL(invoked_command.c_str()) == 0) {
        builtins::mods.add_compiled_file(mod_idx, path.string());
      } else {
        builtins::mods.set_state_at(mod_idx,
                                    builtins::LakeModules::ModState::error);
      }
    });
  }
}

auto CompilationPool::add_task(std::function<void()> &&func) noexcept -> void {
  {
    auto lock = std::unique_lock(task_mtx);
    tasks.emplace_back(std::move(func));
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
} // namespace luamake
