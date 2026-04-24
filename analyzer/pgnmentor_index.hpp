// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// PGN Mentor player index — a static snapshot shipped as
/// `data/pgnmentor_index.json`.
///
/// Each entry pairs a displayable player name with the ZIP
/// filename on pgnmentor.com. The full download URL is formed
/// by prefixing `https://www.pgnmentor.com/players/` to the
/// filename.
///
/// The loader searches a compile-time-baked path
/// (`CHESSERAZADE_SOURCE_DIR/data/pgnmentor_index.json`) and
/// falls back to `./data/pgnmentor_index.json` relative to the
/// current working directory — matching the 1.1.5b
/// magics-lookup pattern.
#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace chesserazade::analyzer {

struct PgnMentorEntry {
    QString name;   ///< Display name, e.g. "Fischer, Robert".
    QString file;   ///< Basename, e.g. "Fischer.zip".

    [[nodiscard]] QString url() const {
        return QStringLiteral("https://www.pgnmentor.com/players/") + file;
    }
};

/// Load the shipped index. Returns `nullopt` if the file is
/// missing or malformed; the UI surfaces the error to the user.
[[nodiscard]] std::optional<std::vector<PgnMentorEntry>>
load_pgnmentor_index();

} // namespace chesserazade::analyzer
