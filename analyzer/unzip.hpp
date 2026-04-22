/// Thin wrapper around the system `unzip` binary.
///
/// 1.3.0 §"unzip" decision: shell out rather than bundle
/// libzip — PGN Mentor archives are plain deflate with no
/// passwords, `unzip` is present on every developer Linux /
/// macOS box, and the failure message guides the user through
/// installing it on the rare box that lacks it.
#pragma once

#include <QString>
#include <expected>

namespace chesserazade::analyzer {

struct UnzipError {
    QString message;
};

/// Extract `archive_path` into `dest_dir`, overwriting
/// existing files. Returns `{}` on success. Fails when `unzip`
/// is not on `PATH` (carries an install-hint message), when
/// the archive is malformed, or when extraction times out.
[[nodiscard]] std::expected<void, UnzipError>
unzip(const QString& archive_path, const QString& dest_dir);

} // namespace chesserazade::analyzer
