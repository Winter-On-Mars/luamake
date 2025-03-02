#include "dependency_graph.hpp"

#include "common.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using std::string, std::string_view;

namespace fs = std::filesystem;

namespace dependency_graph {
auto graph::seen(fs::path const &fpath) const noexcept -> bool {
  return dep_graph.contains(fpath);
}

auto graph::insert(fs::path const &f_name, src_file &&src_file) noexcept
    -> void {
  auto const vec_it = dep_graph.find(f_name);
  if (vec_it == dep_graph.end()) {
    dep_graph.insert(std::make_pair(f_name, std::vector{src_file}));
    return;
  }
  auto &vec = vec_it->second;
  if (std::any_of(vec.begin(), vec.end(), [&src_file](auto &&val) -> bool {
        return src_file.name == val.name;
      })) {
    return;
  } else {
    vec.push_back(src_file);
  }
}

struct File final {
  FILE *handle;

  File(fs::path const f_name) noexcept : handle(fopen(f_name.c_str(), "r")) {}
  ~File() noexcept {
    if (handle != nullptr) {
      fclose(handle);
      handle = nullptr;
    }
  }

  constexpr operator FILE *() noexcept { return handle; }
};

enum class return_t {
  ok,
  cont,
  err,
};

[[nodiscard]]
static auto generate_files_deps(graph &graph, fs::path const &fpath,
                                char *line) noexcept -> return_t;

[[nodiscard]]
static auto generate_impl(graph &graph, fs::path const &fpath, size_t len,
                          char *line) noexcept -> return_t {
  using enum return_t;
  std::cerr << "Calling [generate_impl] with [" << fpath << "]\n";
  auto const include_line = std::string_view(line, len);
  if (include_line.find("#include") == std::string_view::npos) {
    return cont;
  }

  auto const start = include_line.find('"');
  auto const end = include_line.rfind('"');

  if (start == end) {
    return cont;
  }

  std::cout << "{start..end} = ["
            << string(include_line.substr(start + 1, end - start - 1)) << "]\n";

  auto const next_fpath =
      fpath.parent_path() /
      fs::path(include_line.substr(start + 1, end - start - 1));

  if (!next_fpath.has_extension()) {
    error_message(
        "While parsing include directives, unable to find file extension");
    return err;
  }

  if (fpath.extension() == ".cpp" && next_fpath.stem() == fpath.stem()) {
    // guard for recursive includes
    return ok;
  }

  graph.insert(fpath, src_file(next_fpath));
  return generate_files_deps(graph, next_fpath, line);
}

[[nodiscard]]
static auto generate_files_deps(graph &graph, fs::path const &fpath,
                                char *line) noexcept -> return_t {
  using enum return_t;
  std::cerr << "Calling [generate_files_deps] with [" << fpath << "]\n";

  // if header file, also check the .cpp impl
  if (fpath.extension() == ".hpp") {
    auto const impl_fpath =
        fs::path((fpath.parent_path() / fpath.stem()).string() + ".cpp");
    if (!graph.seen(impl_fpath)) {
      auto impl_file = File(impl_fpath);
      if (impl_file != nullptr) { // checking if header is a hol
        impl_file.~File();
        graph.insert(fpath, src_file(impl_fpath));
        if (generate_files_deps(graph, impl_fpath, line) == err) {
          return err;
        }
      }
    }
  }

  auto file = File(fpath);
  if (file == nullptr) {
    ferror_message(
        "While parsing include directives, unable to open file at [%s]",
        fpath.c_str());
    return err;
  }

  size_t len = 0;
  for (auto amount_read = getline(&line, &len, file.handle); amount_read != -1;
       amount_read = getline(&line, &len, file.handle)) {
    switch (generate_impl(graph, fpath, len, line)) {
    case return_t::ok:
      [[fallthrough]];
    case return_t::cont:
      break;
    case return_t::err: {
      return err;
    }
    }
  }

  std::memset(line, 0, len * sizeof(char));
  return ok;
}

auto generate(FILE *root, fs::path const &fpath) noexcept
    -> std::optional<graph> {
  fn_print();
  std::cerr << "Calling [generate] with [" << fpath << "]\n";
  auto graph = dependency_graph::graph();

  auto *line = static_cast<char *>(std::malloc(128 * sizeof(char)));
  if (line == nullptr) {
    ferror_message("Unable to reserve %zu, space for line reading buffer",
                   128 * sizeof(char));
    return std::nullopt;
  }

  line[128 * sizeof(char) - 1] = 0;

  size_t len = 0;
  for (auto amount_read = getline(&line, &len, root); amount_read != -1;
       amount_read = getline(&line, &len, root)) {
    switch (generate_impl(graph, fpath, len, line)) {
    case return_t::ok:
      [[fallthrough]];
    case return_t::cont:
      continue;
    case return_t::err:
      return std::nullopt;
    }
  }

  free(line);

  graph.insert(fpath, src_file(fpath)); // weird work around :)

  return graph;
}
} // namespace dependency_graph
