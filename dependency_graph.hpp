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

class graph final {
  std::unordered_map<std::string, std::vector<src_file>> dep_graph;

public:
  graph() noexcept = default;
  ~graph() noexcept = default;

  graph(graph &&) noexcept = default;
  auto operator=(graph &&) noexcept -> graph & = default;

  graph(graph const &) = delete;
  auto operator=(graph const &) = delete;

  auto insert(std::string &&, src_file &&) noexcept -> void;
};

auto generate(FILE *, std::string_view const, std::filesystem::path) noexcept
    -> std::optional<graph>;

} // namespace dependency_graph
#endif
