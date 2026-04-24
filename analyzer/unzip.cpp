// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "unzip.hpp"

#include <QProcess>
#include <QStandardPaths>

namespace chesserazade::analyzer {

std::expected<void, UnzipError>
unzip(const QString& archive_path, const QString& dest_dir) {
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("unzip"));
    if (exe.isEmpty()) {
        return std::unexpected(UnzipError{
            QStringLiteral(
                "The 'unzip' program was not found on your PATH. "
                "Install it and try again:\n\n"
                "  Debian / Ubuntu:  sudo apt install unzip\n"
                "  Fedora / RHEL:    sudo dnf install unzip\n"
                "  Arch:             sudo pacman -S unzip\n"
                "  macOS:            (preinstalled)\n")});
    }

    QProcess proc;
    // `-o` overwrites existing files without prompting,
    // `-q` keeps the dialog free of stdout spam,
    // `-d` sets the destination directory.
    proc.start(exe, {QStringLiteral("-o"),
                     QStringLiteral("-q"),
                     archive_path,
                     QStringLiteral("-d"), dest_dir});
    if (!proc.waitForFinished(/*msecs=*/60000)) {
        proc.kill();
        proc.waitForFinished(1000);
        return std::unexpected(UnzipError{
            QStringLiteral("unzip timed out after 60 seconds.")});
    }
    if (proc.exitStatus() != QProcess::NormalExit
        || proc.exitCode() != 0) {
        const QString stderr_text =
            QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return std::unexpected(UnzipError{
            QStringLiteral("unzip failed (exit %1): %2")
                .arg(proc.exitCode())
                .arg(stderr_text.isEmpty()
                         ? QStringLiteral("no stderr output")
                         : stderr_text)});
    }
    return {};
}

} // namespace chesserazade::analyzer
