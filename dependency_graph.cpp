#include "dependency_graph.hpp"

#include "common.hpp"

#include <iostream>
#include <string>
#include <string_view>

using std::string, std::string_view;

namespace fs = std::filesystem;

namespace dependency_graph {
struct File final {
  FILE *handle;

  File(fs::path const f_name) noexcept : handle(fopen(f_name.c_str(), "r")) {}
  ~File() noexcept {
    if (handle != nullptr) {
      fclose(handle);
    }
  }
};

/*
[[nodiscard]]
static auto skip_ws(char const *current, char const *const end) noexcept
    -> char const * {
  while (current != end) {
    switch (*current) {
    case ' ':
    case '\n':
    case '\t':
    case '\r':
      ++current;
      break;
    default:
      return current;
    }
  }
  return nullptr; // (?)
}
*/

static auto generate_files_deps(dep_graph &graph, std::string_view const f_name,
                                fs::path rel_path, char *line) noexcept
    -> void /* return code(?)*/ {
  fn_print();
  auto file = File(rel_path / f_name);
  if (file.handle == nullptr) {
    ferror_message(
        "While parsing include directives, unable to open file at %s",
        f_name.data());
    return;
  }

  try {
    size_t len = 0;

    for (auto amount_read = getline(&line, &len, file.handle);
         amount_read != -1; amount_read = getline(&line, &len, file.handle)) {

      auto include_line = std::string_view(line, len);
      if (include_line.find("#include") == std::string_view::npos) {
        continue;
      }

      auto const start = include_line.find('"');
      auto const end = include_line.rfind('"');

      if (start == end) {
        continue;
      }

      auto const name_len = end - start - 1; // end includes the '"'
      if (name_len <= string_view{".xpp"}.length()) {
        error_message("While parsing inclued directives, space between the "
                      "includes is not large enough to include a .cpp or .hpp");
        return;
      }

      auto const name = include_line.substr(start + 1, name_len);
      auto const hpp_pos = name.find(".hpp");
      auto const cpp_pos = name.find(".cpp");

      if (hpp_pos == string_view::npos && cpp_pos == string_view::npos) {
        error_message("While parsing include directives, unable to find '.hpp' "
                      "or '.cpp' in files name");
        return;
      }

      if (hpp_pos != string_view::npos && cpp_pos != string_view::npos) {
        ferror_message("While parsing include directives, found multiple valid "
                       "file extensions on line [%s]",
                       line);
        return;
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
          return;
        }
        auto const next_f_name = include_line.substr(start, name_len);
        graph[string(f_name)].push_back(src_file(string(next_f_name)));
        generate_files_deps(graph, next_f_name, rel_path, line);
      } else {
        auto const last_cpp_pos = name.rfind(".cpp");
        if (last_cpp_pos != cpp_pos) {
          ferror_message(
              "While parsing include directives, found multiple instances of "
              "'.hpp' file extension in line [%s]" NL
              "\tIf this is intentional, maybe as a part of some code "
              "generation, open an issue on gh and we can work to resolve it",
              line);
          return;
        }
        auto const next_f_name = include_line.substr(start, name_len);
        graph[string(f_name)].push_back(src_file(string(next_f_name)));
        generate_files_deps(graph, next_f_name, rel_path, line);
      }
    }
  } catch (...) {
    ferror_message("Something went wrong while attempting to generate "
                   "dependencies of [%s]",
                   f_name.data());
  }
  free(line);
}

auto generate(FILE *root, string_view const root_name,
              fs::path rel_path) noexcept -> std::optional<dep_graph> {
  fn_print();
  auto graph = dep_graph{};

  auto *line = static_cast<char *>(malloc(128 * sizeof(char)));
  if (line == nullptr) {
    ferror_message("Unable to reserve %zu, space for line reading buffer",
                   128 * sizeof(char));
    return std::nullopt;
  }

  try {
    size_t len = 0;

    for (auto amount_read = getline(&line, &len, root); amount_read != -1;
         amount_read = getline(&line, &len, root)) {

      auto include_line = string_view(line, len);

      if (include_line.find("#include") == string_view::npos) {
        continue;
      }

      auto const start = include_line.find('"');
      auto const end = include_line.rfind('"');

      if (start == end) {
        continue;
      }

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
                       line);
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
        graph[string(name)].push_back(src_file(string(root_name)));
        generate_files_deps(graph, name, rel_path, line);
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
        graph[string(name)].push_back(src_file(string(root_name)));
        generate_files_deps(graph, name, rel_path, line);
      }
    }
  } catch (...) {
    error_message("Something went wrong while generating dependencies\n\tAn "
                  "exception was thrown");
  }

  for (auto const &files : fs::directory_iterator(rel_path)) {
    std::cout << "[" << files << "]\n";
  }
  std::cout << std::flush;

  free(line);

  return graph;
}
} // namespace dependency_graph
