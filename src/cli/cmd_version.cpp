#include "cli/cmd_version.hpp"

#include <iostream>

namespace chesserazade::cli {

namespace {

constexpr std::string_view PROJECT_NAME = "chesserazade";
constexpr std::string_view PROJECT_VERSION = "0.6.0";

} // namespace

int cmd_version(std::span<const std::string_view> /*args*/) {
    std::cout << PROJECT_NAME << ' ' << PROJECT_VERSION << '\n';
    return 0;
}

} // namespace chesserazade::cli
