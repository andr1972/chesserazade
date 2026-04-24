#include "add_bookmark_dialog.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace chesserazade::analyzer {

namespace {

/// One-line summary of the saved position — "Fischer vs
/// Spassky, 1972.07.11, 17 w" or similar. Goes into the read-
/// only header block so the user sees exactly what's being
/// pinned before they pick a label.
[[nodiscard]] QString summary_line(const Bookmark& bm) {
    const QString who = QStringLiteral("%1 vs %2")
                            .arg(bm.white.isEmpty() ? QStringLiteral("?")
                                                    : bm.white)
                            .arg(bm.black.isEmpty() ? QStringLiteral("?")
                                                    : bm.black);
    QString out = who;
    if (!bm.date.isEmpty()) {
        out += QStringLiteral(", ") + bm.date;
    }
    out += QStringLiteral(" · ") + ply_to_notation(bm.ply);
    return out;
}

} // namespace

AddBookmarkDialog::AddBookmarkDialog(const Bookmark& prefill,
                                     const QStringList& existing_folders,
                                     QWidget* parent)
    : QDialog(parent), prefill_(prefill) {
    setWindowTitle(tr("Add bookmark"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);

    // Read-only header — what's being saved, mechanically.
    auto* summary = new QLabel(
        tr("<b>Source:</b> %1<br><b>Game:</b> %2")
            .arg(prefill_.zip.isEmpty() ? tr("(unknown)")
                                        : prefill_.zip)
            .arg(summary_line(prefill_)),
        this);
    summary->setWordWrap(true);
    summary->setTextFormat(Qt::RichText);
    layout->addWidget(summary);

    auto* hline = new QFrame(this);
    hline->setFrameShape(QFrame::HLine);
    hline->setFrameShadow(QFrame::Sunken);
    layout->addWidget(hline);

    auto* form = new QFormLayout;

    label_edit_ = new QLineEdit(this);
    connect(label_edit_, &QLineEdit::textChanged,
            this, [this](const QString&) { update_ok_enabled(); });
    form->addRow(tr("&Label:"), label_edit_);

    folder_combo_ = new QComboBox(this);
    folder_combo_->setEditable(true);
    // Blank entry = "no folder". Selecting it (or leaving
    // the field empty) saves the bookmark at the top level.
    folder_combo_->addItem(QString{});
    folder_combo_->addItems(existing_folders);
    folder_combo_->setCurrentText(prefill_.folder);
    folder_combo_->setToolTip(tr(
        "Single-level folder. Pick an existing one or type a "
        "new name — a folder materialises as soon as any "
        "bookmark names it. Leave empty for the top level."));
    form->addRow(tr("&Folder:"), folder_combo_);

    comment_edit_ = new QPlainTextEdit(this);
    comment_edit_->setPlaceholderText(
        tr("Free-form notes. Editable later — does not have to "
           "be filled in now."));
    comment_edit_->setTabChangesFocus(true);
    comment_edit_->setPlainText(prefill_.comment);
    form->addRow(tr("&Comment:"), comment_edit_);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_btn_ = buttons->button(QDialogButtonBox::Ok);
    ok_btn_->setText(tr("&Save"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    label_edit_->setFocus();
    update_ok_enabled();
}

void AddBookmarkDialog::update_ok_enabled() {
    // Label is the only required field — an empty bookmark
    // would be useless in the list view. Zero-length folder
    // and comment are fine.
    ok_btn_->setEnabled(!label_edit_->text().trimmed().isEmpty());
}

Bookmark AddBookmarkDialog::result() const {
    Bookmark b = prefill_;
    b.label   = label_edit_->text().trimmed();
    b.folder  = folder_combo_->currentText().trimmed();
    b.comment = comment_edit_->toPlainText();
    b.created_ms = QDateTime::currentMSecsSinceEpoch();
    return b;
}

} // namespace chesserazade::analyzer
