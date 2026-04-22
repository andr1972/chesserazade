#include "game_list_view.hpp"

#include "game_list_model.hpp"

#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QModelIndex>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

namespace chesserazade::analyzer {

GameListView::GameListView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);

    model_ = new GameListModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setSortCaseSensitivity(Qt::CaseInsensitive);

    table_ = new QTableView(this);
    table_->setModel(proxy_);
    table_->setSortingEnabled(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setDefaultSectionSize(
        table_->fontMetrics().height() + 6);
    connect(table_, &QTableView::activated,
            this, &GameListView::on_activated);
    layout->addWidget(table_, /*stretch=*/1);

    status_->setText(tr("No PGN loaded."));
}

bool GameListView::load(const QString& pgn_path) {
    source_path_ = pgn_path;
    pgn_bytes_.clear();
    games_.clear();
    model_->set_games(nullptr);

    QFile f(pgn_path);
    if (!f.open(QIODevice::ReadOnly)) {
        status_->setText(tr("Could not open %1 for reading.").arg(pgn_path));
        return false;
    }
    const QByteArray bytes = f.readAll();
    pgn_bytes_.assign(bytes.constData(),
                      bytes.constData() + bytes.size());

    games_ = index_games(pgn_bytes_);
    if (games_.empty()) {
        status_->setText(tr(
            "No games found in %1 (the file is either empty "
            "or does not follow the [Event ...] header "
            "convention).").arg(pgn_path));
        return false;
    }

    model_->set_games(&games_);
    table_->resizeColumnsToContents();
    // Default to oldest-first on Date. PGN Mentor dates are
    // sometimes "YYYY.??.??" but the year still sorts correctly
    // lexicographically so this gives useful ordering.
    table_->sortByColumn(GameListModel::ColDate, Qt::AscendingOrder);

    status_->setText(tr(
        "%1 — %2 game(s) indexed. Double-click a row to open.")
            .arg(QFileInfo(pgn_path).fileName())
            .arg(games_.size()));
    return true;
}

int GameListView::source_row(const QModelIndex& idx) const {
    if (proxy_ == nullptr) return idx.row();
    return proxy_->mapToSource(idx).row();
}

void GameListView::on_activated(const QModelIndex& idx) {
    if (!idx.isValid()) return;
    const int row = source_row(idx);
    if (row < 0) return;
    const auto r = static_cast<std::size_t>(row);
    if (r >= games_.size()) return;

    const PgnGameHeader& g = games_[r];
    if (g.offset + g.length > pgn_bytes_.size()) return;

    const std::string_view slice(
        pgn_bytes_.data() + g.offset, g.length);
    const QString pgn_text = QString::fromUtf8(
        slice.data(), static_cast<qsizetype>(slice.size()));
    const QString label = QStringLiteral("%1 — %2, %3")
        .arg(QString::fromStdString(g.white))
        .arg(QString::fromStdString(g.black))
        .arg(QString::fromStdString(g.date));

    emit game_chosen(pgn_text, label);
}

} // namespace chesserazade::analyzer
