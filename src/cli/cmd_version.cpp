// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "cli/cmd_version.hpp"

#include <iostream>

namespace chesserazade::cli {

namespace {

constexpr std::string_view PROJECT_NAME = "chesserazade";
constexpr std::string_view PROJECT_VERSION = "1.3.0";

} // namespace

int cmd_version(std::span<const std::string_view> /*args*/) {
    std::cout << PROJECT_NAME << ' ' << PROJECT_VERSION << '\n';
    return 0;
}

} // namespace chesserazade::cli
