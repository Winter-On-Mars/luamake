#ifndef __DEP_GRAPH_HPP
#define __DEP_GRAPH_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dependency_graph {

struct src_file final {
  // std::size_t hash;
  std::string name;
};

using dep_graph = std::unordered_map<std::string, std::vector<src_file>>;

auto generate(FILE *, std::string_view const, std::filesystem::path) noexcept
    -> std::optional<dep_graph>;

} // namespace dependency_graph
#endif
