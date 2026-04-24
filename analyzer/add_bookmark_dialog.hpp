// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Modal for creating a new bookmark. The caller pre-fills the
/// game-identification fields (zip, white/black/date/event/round,
/// ply) from whichever view it was invoked from; the dialog
/// only takes user input for the three editable fields:
/// `label`, `folder`, and `comment`.
///
/// Folders are free text; the combobox is prepopulated with the
/// names currently in use (passed in by the caller) but the user
/// can type a new one. Pressing OK with an unknown folder just
/// writes it as a fresh folder — no confirmation dialog, no
/// separate "create folder" step.
#pragma once

#include "bookmarks.hpp"

#include <QDialog>
#include <QString>
#include <QStringList>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace chesserazade::analyzer {

class AddBookmarkDialog final : public QDialog {
    Q_OBJECT
public:
    /// `prefill` supplies everything except the user-editable
    /// bits — zip + STR + ply are stored on the dialog and
    /// merged into `result()` at OK-time.
    AddBookmarkDialog(const Bookmark& prefill,
                      const QStringList& existing_folders,
                      QWidget* parent = nullptr);

    /// Completed bookmark. Safe to read only after `exec()`
    /// returned `QDialog::Accepted`.
    [[nodiscard]] Bookmark result() const;

private:
    void update_ok_enabled();

    Bookmark prefill_;
    QLineEdit*      label_edit_   = nullptr;
    QComboBox*      folder_combo_ = nullptr;
    QPlainTextEdit* comment_edit_ = nullptr;
    QPushButton*    ok_btn_       = nullptr;
};

} // namespace chesserazade::analyzer
