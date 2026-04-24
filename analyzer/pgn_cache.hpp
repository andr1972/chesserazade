/// PGN archive cache helpers — locate / download / unzip
/// pgnmentor player archives.
///
/// The pipeline is:
///   zip filename (e.g. "Fischer.zip")
///     → cache path for the zip,
///     → cache path for the extracted pgn,
///     → download + unzip on miss, yielding the pgn path.
///
/// Both the Fetch dialog and the Bookmarks flow need the same
/// "ensure the pgn is on disk, prompt and download if not"
/// behaviour, so the UI loop (confirm → fetch → save → unzip
/// → error-if-missing) lives here behind `ensure_pgn`.
#pragma once

#include <QString>

#include <optional>

class QWidget;

namespace chesserazade::analyzer {

/// XDG-style cache root:
/// `$XDG_CACHE_HOME/chesserazade`, or `$HOME/.cache/chesserazade`,
/// or Qt's `CacheLocation` as the last fallback.
[[nodiscard]] QString cache_dir();

/// Absolute path where a given pgnmentor `.zip` (e.g.
/// "Fischer.zip") lives on disk. Not guaranteed to exist.
[[nodiscard]] QString zip_path_for_zip(const QString& zip_file);

/// Absolute path where the extracted PGN for a given
/// pgnmentor `.zip` lives on disk. Not guaranteed to exist.
/// PGN Mentor zips contain a single `<stem>.pgn`; we derive
/// that name without opening the archive.
[[nodiscard]] QString pgn_path_for_zip(const QString& zip_file);

/// Ensure the extracted PGN is on disk, downloading the
/// archive via the pgnmentor URL if needed. Puts up a
/// confirmation dialog before any network traffic; reports
/// errors via `QMessageBox` on `parent`.
///
/// Returns the path to the ready-to-read PGN on success;
/// nullopt if the user cancelled the download or any step
/// (network, write, unzip) failed.
[[nodiscard]] std::optional<QString>
ensure_pgn(const QString& zip_file, QWidget* parent);

} // namespace chesserazade::analyzer
