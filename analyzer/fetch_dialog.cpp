#include "fetch_dialog.hpp"

#include "pgnmentor_index.hpp"
#include "unzip.hpp"

#include <chesserazade/net_fetcher.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>

namespace chesserazade::analyzer {

namespace {

/// XDG-style cache dir. Mirrors the CLI's `cmd_fetch.cpp`
/// behaviour so both tools share a single cache footprint.
[[nodiscard]] QString resolve_cache_dir() {
    const QByteArray xdg = qgetenv("XDG_CACHE_HOME");
    if (!xdg.isEmpty()) {
        return QString::fromUtf8(xdg) + QStringLiteral("/chesserazade");
    }
    const QByteArray home = qgetenv("HOME");
    if (!home.isEmpty()) {
        return QString::fromUtf8(home)
             + QStringLiteral("/.cache/chesserazade");
    }
    return QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
}

} // namespace

FetchDialog::FetchDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Fetch — PGN Mentor"));
    resize(520, 520);

    auto* layout = new QVBoxLayout(this);

    auto* header = new QLabel(
        tr("Pick a player. Archives already in the local cache "
           "are marked with a folder icon and will open "
           "without re-downloading."),
        this);
    header->setWordWrap(true);
    layout->addWidget(header);

    auto* filter_row = new QHBoxLayout;
    filter_edit_ = new QLineEdit(this);
    filter_edit_->setPlaceholderText(tr("filter (first or last name)"));
    filter_edit_->setClearButtonEnabled(true);
    filter_edit_->installEventFilter(this);
    connect(filter_edit_, &QLineEdit::textChanged,
            this, [this](const QString&) { rebuild_list(); });
    filter_row->addWidget(filter_edit_, /*stretch=*/1);

    only_cached_check_ = new QCheckBox(tr("only cached"), this);
    only_cached_check_->setToolTip(tr(
        "Hide players whose archive is not yet in the local "
        "cache. Useful for picking up where you left off without "
        "scrolling past remote-only entries."));
    connect(only_cached_check_, &QCheckBox::toggled,
            this, [this](bool) { rebuild_list(); });
    filter_row->addWidget(only_cached_check_);

    layout->addLayout(filter_row);

    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(list_, &QListWidget::currentItemChanged,
            this, &FetchDialog::on_selection_changed);
    connect(list_, &QListWidget::itemDoubleClicked,
            this, [this]() { on_open_clicked(); });
    layout->addWidget(list_, /*stretch=*/1);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* buttons = new QDialogButtonBox(this);
    open_btn_ = buttons->addButton(tr("&Open"),
                                   QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    open_btn_->setEnabled(false);
    connect(open_btn_, &QPushButton::clicked,
            this, &FetchDialog::on_open_clicked);
    connect(buttons->button(QDialogButtonBox::Cancel),
            &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttons);

    const auto loaded = load_pgnmentor_index();
    if (!loaded) {
        status_->setText(tr(
            "<b>Index unavailable.</b> Could not read "
            "<code>data/pgnmentor_index.json</code>. "
            "Make sure the file is present in the repo root "
            "or next to the binary."));
        list_->setEnabled(false);
        return;
    }
    entries_ = *loaded;
    rebuild_list();
}

QString FetchDialog::cache_dir() const {
    return resolve_cache_dir();
}

QString FetchDialog::zip_path_for(const PgnMentorEntry& e) const {
    return cache_dir() + QLatin1Char('/') + e.file;
}

QString FetchDialog::pgn_path_for(const PgnMentorEntry& e) const {
    // PGN Mentor zips contain a single `<Name>.pgn`. We derive
    // the expected filename from the zip's stem so we can tell
    // cached entries apart without actually opening the zip.
    const QString stem = QFileInfo(e.file).completeBaseName();
    return cache_dir() + QLatin1Char('/') + stem
         + QStringLiteral(".pgn");
}

bool FetchDialog::entry_matches(const PgnMentorEntry& e) const {
    if (only_cached_check_ != nullptr
        && only_cached_check_->isChecked()
        && !QFileInfo::exists(pgn_path_for(e))) {
        return false;
    }
    const QString needle = filter_edit_ != nullptr
        ? filter_edit_->text().trimmed()
        : QString{};
    if (needle.isEmpty()) return true;
    // `name` is already "Last, First" so substring-match against
    // it finds hits on either half ("polgar" → "Polgar, Judit",
    // "judit" → same). Case-insensitive; no regex to keep the
    // search feeling instant on every keystroke.
    return e.name.contains(needle, Qt::CaseInsensitive);
}

void FetchDialog::rebuild_list() {
    list_->clear();
    QStyle* style = QApplication::style();
    const QIcon cached_icon =
        style->standardIcon(QStyle::SP_DirIcon);
    const QIcon remote_icon =
        style->standardIcon(QStyle::SP_FileIcon);

    // Filtering breaks the row-index ↔ entries_-index parity,
    // so we stash the original index on each item via UserRole;
    // the open/selection handlers read that back.
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        if (!entry_matches(e)) continue;
        const bool cached = QFileInfo::exists(pgn_path_for(e));
        auto* item = new QListWidgetItem(list_);
        item->setText(e.name
                      + (cached ? tr("   [cached]") : QString{}));
        item->setIcon(cached ? cached_icon : remote_icon);
        item->setToolTip(e.url());
        item->setData(Qt::UserRole, static_cast<int>(i));
    }
}

bool FetchDialog::eventFilter(QObject* obj, QEvent* e) {
    if (obj == filter_edit_ && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape
            && !filter_edit_->text().isEmpty()) {
            filter_edit_->clear();
            return true; // consumed — do not close the dialog.
        }
    }
    return QDialog::eventFilter(obj, e);
}

void FetchDialog::on_selection_changed() {
    QListWidgetItem* item = list_->currentItem();
    open_btn_->setEnabled(item != nullptr);
    if (item != nullptr) {
        const int idx = item->data(Qt::UserRole).toInt();
        const auto& e = entries_[static_cast<std::size_t>(idx)];
        const bool cached = QFileInfo::exists(pgn_path_for(e));
        if (cached) {
            status_->setText(tr("Cached: %1 — will open without "
                                "downloading.").arg(pgn_path_for(e)));
        } else {
            status_->setText(tr("Will download from: %1").arg(e.url()));
        }
    }
}

void FetchDialog::on_open_clicked() {
    QListWidgetItem* item = list_->currentItem();
    if (item == nullptr) return;
    const int idx = item->data(Qt::UserRole).toInt();
    if (idx < 0 || static_cast<std::size_t>(idx) >= entries_.size()) return;
    const PgnMentorEntry& e = entries_[static_cast<std::size_t>(idx)];

    const QString pgn_path = pgn_path_for(e);
    if (QFileInfo::exists(pgn_path)) {
        selected_pgn_ = pgn_path;
        emit pgn_ready(pgn_path);
        accept();
        return;
    }

    // Ensure cache dir exists.
    const QString cache = cache_dir();
    QDir().mkpath(cache);

    // Confirm the network call — same policy as the CLI
    // fetch: no background traffic, user sees the URL.
    const auto btn = QMessageBox::question(
        this,
        tr("Download?"),
        tr("About to download:\n\n%1\n\nSize varies "
           "(tens of KB to a few MB). Proceed?")
            .arg(e.url()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (btn != QMessageBox::Yes) return;

    status_->setText(tr("Downloading %1 …").arg(e.file));
    QApplication::processEvents();

    CurlFetcher fetcher;
    const auto r = fetcher.fetch(e.url().toStdString());
    if (!r) {
        QMessageBox::warning(
            this, tr("Download failed"),
            QString::fromStdString(r.error().message));
        status_->clear();
        return;
    }

    const QString zip_path = zip_path_for(e);
    QFile zf(zip_path);
    if (!zf.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(
            this, tr("Save failed"),
            tr("Could not open %1 for writing.").arg(zip_path));
        status_->clear();
        return;
    }
    zf.write(r->data(), static_cast<qint64>(r->size()));
    zf.close();

    status_->setText(tr("Unzipping %1 …").arg(e.file));
    QApplication::processEvents();

    const auto uz = unzip(zip_path, cache);
    if (!uz) {
        QMessageBox::warning(
            this, tr("Unzip failed"), uz.error().message);
        status_->clear();
        return;
    }

    if (!QFileInfo::exists(pgn_path)) {
        QMessageBox::warning(
            this, tr("PGN missing"),
            tr("Download succeeded but the expected file\n"
               "%1\nwas not produced by unzip. The archive "
               "may use a non-standard layout.").arg(pgn_path));
        status_->clear();
        return;
    }

    selected_pgn_ = pgn_path;
    emit pgn_ready(pgn_path);
    accept();
}

} // namespace chesserazade::analyzer
