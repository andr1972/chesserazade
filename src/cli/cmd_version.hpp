/// `chesserazade version` — prints the project version string.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_version(std::span<const std::string_view> args);

} // namespace chesserazade::cli
