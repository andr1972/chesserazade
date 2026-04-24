// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `chesserazade version` — prints the project version string.
#pragma once

#include <span>
#include <string_view>

namespace chesserazade::cli {

int cmd_version(std::span<const std::string_view> args);

} // namespace chesserazade::cli
