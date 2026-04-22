#include "fetch_dialog.hpp"

#include "pgnmentor_index.hpp"
#include "unzip.hpp"

#include <chesserazade/net_fetcher.hpp>

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
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

void FetchDialog::rebuild_list() {
    list_->clear();
    QStyle* style = QApplication::style();
    const QIcon cached_icon =
        style->standardIcon(QStyle::SP_DirIcon);
    const QIcon remote_icon =
        style->standardIcon(QStyle::SP_FileIcon);

    for (const auto& e : entries_) {
        const bool cached = QFileInfo::exists(pgn_path_for(e));
        auto* item = new QListWidgetItem(list_);
        item->setText(e.name
                      + (cached ? tr("   [cached]") : QString{}));
        item->setIcon(cached ? cached_icon : remote_icon);
        item->setToolTip(e.url());
    }
}

void FetchDialog::on_selection_changed() {
    open_btn_->setEnabled(list_->currentItem() != nullptr);
    if (list_->currentItem() != nullptr) {
        const int row = list_->currentRow();
        const auto& e = entries_[static_cast<std::size_t>(row)];
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
    const int row = list_->currentRow();
    if (row < 0
        || static_cast<std::size_t>(row) >= entries_.size()) return;
    const PgnMentorEntry& e = entries_[static_cast<std::size_t>(row)];

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
