#include "dependency_graph.hpp"

#include "common.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using std::string, std::string_view;

namespace fs = std::filesystem;

namespace dependency_graph {
auto graph::insert(string &&f_name, src_file &&src_file) noexcept -> void {
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
static auto generate_files_deps(graph &graph, string_view const f_name,
                                fs::path const &rel_path, char *line) noexcept
    -> return_t;

[[nodiscard]]
static auto generate_impl(graph &graph, size_t len, char *line,
                          fs::path const &rel_path,
                          string_view const f_name) noexcept -> return_t {
  using enum return_t;
  auto include_line = std::string_view(line, len);
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

  auto const name_len = end - start - 1; // end includes the '"'
  if (name_len <= string_view{".xpp"}.length()) {
    error_message("While parsing inclued directives, space between the "
                  "includes is not large enough to include a .cpp or .hpp");
    return err;
  }

  auto const name = include_line.substr(start + 1, name_len);
  auto const hpp_pos = name.find(".hpp");
  auto const cpp_pos = name.find(".cpp");

  if (hpp_pos == string_view::npos && cpp_pos == string_view::npos) {
    error_message("While parsing include directives, unable to find '.hpp' "
                  "or '.cpp' in files name");
    return err;
  }

  if (hpp_pos != string_view::npos && cpp_pos != string_view::npos) {
    ferror_message("While parsing include directives, found multiple valid "
                   "file extensions on line [%s]",
                   line);
    return err;
  }

  if (hpp_pos != string_view::npos) {
    auto const last_hpp_pos = name.rfind(".hpp");
    if (last_hpp_pos != hpp_pos) {
      ferror_message(
          "While parsing include directives, found multiple instances of "
          "'.hpp' file extension in line [%s]" NL
          "\tIf this is intentional, maybe as a part of some code "
          "generation, open an issue on gh and we can work to resolve it",
          line);
      return err;
    }
    if (name.substr(0, last_hpp_pos) !=
        f_name.substr(0, f_name.rfind(".cpp"))) {
      // graph.insert(string(f_name), src_file(string(name)));
      graph.insert(string(name), src_file(string(f_name)));
      if (generate_files_deps(graph, name, rel_path, line) == err) {
        return err;
      }
    }
  } else {
    auto const last_cpp_pos = name.rfind(".cpp");
    if (last_cpp_pos != cpp_pos) {
      ferror_message(
          "While parsing include directives, found multiple instances of "
          "'.hpp' file extension in line [%s]" NL
          "\tIf this is intentional, maybe as a part of some code "
          "generation, open an issue on gh and we can work to resolve it",
          line);
      return err;
    }
    graph.insert(string(f_name), src_file(string(name)));
    // graph.insert(string(name), src_file(string(f_name)));
    if (generate_files_deps(graph, name, rel_path, line) == err) {
      return err;
    }
  }
  return err;
}

// TODO: extract the large bits of logic from this function and the other
// generate function
[[nodiscard]]
static auto generate_files_deps(graph &graph, string_view const f_name,
                                fs::path const &rel_path, char *line) noexcept
    -> return_t {
  using enum return_t;
  std::cerr << "Calling [generate_files_deps] with [" << f_name << "]\n";

  // if header file, also check the .cpp impl
  if (auto const hpp_pos = f_name.find(".hpp"); hpp_pos != string_view::npos) {
    auto const impl_file = string(f_name.substr(0, hpp_pos)) + ".cpp";
    auto impl_file_check = File(rel_path / impl_file);
    if (impl_file_check != nullptr) {
      impl_file_check.~File();
      graph.insert(string(f_name), src_file(string(impl_file)));
      if (generate_files_deps(graph, impl_file, rel_path, line) == err) {
        return err;
      }
    }
  }

  auto file = File(rel_path / f_name);
  if (file == nullptr) {
    ferror_message(
        "While parsing include directives, unable to open file at [%s]",
        f_name.data());
    return err;
  }

  size_t len = 0;
  for (auto amount_read = getline(&line, &len, file.handle); amount_read != -1;
       amount_read = getline(&line, &len, file.handle)) {
    switch (generate_impl(graph, len, line, rel_path, f_name)) {
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

auto generate(FILE *root, string_view const root_name,
              fs::path const &rel_path) noexcept -> std::optional<graph> {
  fn_print();
  auto graph = dependency_graph::graph();

  auto *line = static_cast<char *>(std::malloc(128 * sizeof(char)));
  if (line == nullptr) {
    ferror_message("Unable to reserve %zu, space for line reading buffer",
                   128 * sizeof(char));
    return std::nullopt;
  }

  size_t len = 0;

  for (auto amount_read = getline(&line, &len, root); amount_read != -1;
       amount_read = getline(&line, &len, root)) {

    auto const include_line = string_view(line, len);

    if (include_line.find("#include") == string_view::npos) {
      continue;
    }

    auto const start = include_line.find('"');
    auto const end = include_line.rfind('"');

    if (start == end) {
      continue;
    }

    std::cout << "{start..end} = ["
              << string(include_line.substr(start + 1, end - start)) << "]\n";

    auto const name_len = end - start - 1; // end includes the '"'
    if (name_len <= string_view{".xpp"}.length()) {
      error_message("While parsing include directives, space between the "
                    "includes is not large enough to include a .cpp or .hpp");
      return std::nullopt;
    }

    auto const name = include_line.substr(start + 1, name_len);
    auto const hpp_pos = name.find(".hpp");
    auto const cpp_pos = name.find(".cpp");
    if (hpp_pos == string_view::npos && cpp_pos == string_view::npos) {
      error_message("While parsing include directives, unable to find .hpp "
                    "or .cpp in the files name");
      return std::nullopt;
    }

    if (hpp_pos != string_view::npos && cpp_pos != string_view::npos) {
      // TODO: remove the NL char from the line buf bc it kinda fucks error
      // messages up :(
      ferror_message("While parsing include directives, found multiple valid "
                     "file extensions on line [%s]",
                     name.data());
      return std::nullopt;
    }

    if (hpp_pos != string_view::npos) {
      auto const last_hpp_pos = name.rfind(".hpp");
      if (last_hpp_pos != hpp_pos) {
        ferror_message(
            "While parsing include directives, found multiple instances of "
            "'.hpp' file extension in line [%s]" NL
            "\tIf this is intentional, maybe as a part of some code "
            "generation, open an issue on gh and we can work to resolve it",
            line);
        return std::nullopt;
      }
      graph.insert(string(name), src_file(string(root_name)));
      if (generate_files_deps(graph, name, rel_path, line) == return_t::err) {
        return std::nullopt;
      }
    } else {
      auto const last_cpp_pos = name.rfind(".cpp");
      if (last_cpp_pos != cpp_pos) {
        ferror_message(
            "While parsing include directives, found multiple instances of "
            "'.cpp' file extension in line [%s]" NL
            "\tIf this is intentional, maybe as a part of some code "
            "generation, open an issue on gh and we can work to resolve it",
            line);
        return std::nullopt;
      }
      graph.insert(string(name), src_file(string(root_name)));
      if (generate_files_deps(graph, name, rel_path, line) == return_t::err) {
        return std::nullopt;
      }
    }
  }

  free(line);

  return graph;
}
} // namespace dependency_graph
