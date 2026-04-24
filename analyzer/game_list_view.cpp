// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "game_list_view.hpp"

#include "game_list_model.hpp"

#include <chesserazade/game_index.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QModelIndex>
#include <QProgressDialog>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <string>

namespace chesserazade::analyzer {

// ---------------------------------------------------------------------------
// Proxy — two filters joined by AND:
//   * name:  case-insensitive substring, either White or Black matches.
//   * year:  substring of the leading "YYYY" of the Date column.
// Both empty → everything visible.
// ---------------------------------------------------------------------------
class GameFilterProxy final : public QSortFilterProxyModel {
public:
    explicit GameFilterProxy(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent) {}

    void set_name(const QString& s) {
        if (s == name_) return;
        name_ = s;
        invalidateFilter();
    }
    void set_year(const QString& s) {
        if (s == year_) return;
        year_ = s;
        invalidateFilter();
    }
    void set_mate_only(bool on) {
        if (on == mate_only_) return;
        mate_only_ = on;
        invalidateFilter();
    }
    void set_up_only(bool on) {
        if (on == up_only_) return;
        up_only_ = on;
        invalidateFilter();
    }
    void set_kf_only(bool on) {
        if (on == kf_only_) return;
        kf_only_ = on;
        invalidateFilter();
    }
    void set_sac_only(bool on) {
        if (on == sac_only_) return;
        sac_only_ = on;
        invalidateFilter();
    }
    void set_sac_won_only(bool on) {
        if (on == sac_won_only_) return;
        sac_won_only_ = on;
        invalidateFilter();
    }

protected:
    [[nodiscard]] bool filterAcceptsRow(int row,
                                        const QModelIndex& parent)
        const override {
        const QAbstractItemModel* m = sourceModel();
        if (m == nullptr) return true;
        if (!name_.isEmpty()) {
            const QString w = m->data(m->index(row,
                                               GameListModel::ColWhite,
                                               parent)).toString();
            const QString b = m->data(m->index(row,
                                               GameListModel::ColBlack,
                                               parent)).toString();
            if (!w.contains(name_, Qt::CaseInsensitive)
                && !b.contains(name_, Qt::CaseInsensitive)) {
                return false;
            }
        }
        if (!year_.isEmpty()) {
            const QString d = m->data(m->index(row,
                                               GameListModel::ColDate,
                                               parent)).toString();
            // Match against the first 4 chars (YYYY) — PGN dates
            // are like "1972.07.11" or "1972.??.??", both of
            // which start with the year, so a substring check
            // on the prefix suffices.
            if (!d.left(4).contains(year_, Qt::CaseInsensitive)) {
                return false;
            }
        }
        if (mate_only_) {
            const QString ek = m->data(m->index(row,
                                                GameListModel::ColEnd,
                                                parent)).toString();
            if (ek != QStringLiteral("#")) return false;
        }
        if (up_only_) {
            const QString up = m->data(m->index(row,
                                                GameListModel::ColUP,
                                                parent)).toString();
            if (up.isEmpty()) return false;
        }
        if (kf_only_) {
            const QVariant v = m->data(m->index(row,
                                                GameListModel::ColKF,
                                                parent));
            if (!v.isValid() || v.toString().isEmpty()) return false;
        }
        if (sac_only_) {
            const QString sv = m->data(m->index(row,
                                                GameListModel::ColSac,
                                                parent)).toString();
            if (sv.isEmpty()) return false;
        }
        if (sac_won_only_) {
            // Drop into the source model for fields we don't
            // surface as columns (ply parity → sacrificer's
            // color, cross-checked against the Result tag).
            const auto* glm = qobject_cast<const GameListModel*>(m);
            if (glm == nullptr) return false;
            const GameRecord* rec = glm->record_at(row);
            if (rec == nullptr) return false;
            if (rec->material_sacs.empty()) return false;
            const std::string& res = rec->header.result;
            // Map game result to the side that won.
            // "1-0" → white, "0-1" → black, else → no winner.
            const bool white_won = (res == "1-0");
            const bool black_won = (res == "0-1");
            if (!white_won && !black_won) return false;
            bool any_match = false;
            for (const auto& s : rec->material_sacs) {
                // 1-based ply parity: odd = white moved, even = black.
                const bool sacrificer_is_white = (s.ply % 2 == 1);
                if ((sacrificer_is_white && white_won)
                    || (!sacrificer_is_white && black_won)) {
                    any_match = true;
                    break;
                }
            }
            if (!any_match) return false;
        }
        return true;
    }

private:
    QString name_;
    QString year_;
    bool    mate_only_   = false;
    bool    up_only_     = false;
    bool    kf_only_     = false;
    bool    sac_only_    = false;
    bool    sac_won_only_ = false;
};

GameListView::GameListView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* filter_row = new QHBoxLayout;
    name_filter_ = new QLineEdit(this);
    name_filter_->setPlaceholderText(tr("filter by player name"));
    name_filter_->setClearButtonEnabled(true);
    name_filter_->installEventFilter(this);
    connect(name_filter_, &QLineEdit::textChanged,
            this, [this](const QString&) { on_name_filter_changed(); });
    filter_row->addWidget(name_filter_, /*stretch=*/2);

    year_filter_ = new QComboBox(this);
    year_filter_->setEditable(true);
    year_filter_->setInsertPolicy(QComboBox::NoInsert);
    year_filter_->lineEdit()->setPlaceholderText(tr("year"));
    year_filter_->lineEdit()->setClearButtonEnabled(true);
    year_filter_->lineEdit()->installEventFilter(this);
    year_filter_->setToolTip(tr(
        "Filter by year. The dropdown lists only years that "
        "have at least one game under the current name filter, "
        "so the list shrinks as you narrow the name."));
    connect(year_filter_, &QComboBox::currentTextChanged,
            this, [this](const QString&) { on_year_filter_changed(); });
    filter_row->addWidget(year_filter_, /*stretch=*/1);

    mate_filter_ = new QCheckBox(tr("mate only"), this);
    mate_filter_->setToolTip(tr(
        "Show only games that ended by checkmate — detected "
        "by replaying the move list and checking the final "
        "position (no legal reply while the side to move is "
        "in check). Independent of the PGN Result tag; hides "
        "resignations, agreed draws, timeouts and "
        "adjudications."));
    connect(mate_filter_, &QCheckBox::toggled,
            this, [this](bool on) { proxy_->set_mate_only(on); });
    filter_row->addWidget(mate_filter_);

    up_filter_ = new QCheckBox(tr("underpromotion"), this);
    up_filter_->setToolTip(tr(
        "Show only games with at least one non-queen "
        "promotion (knight / bishop / rook). Rare — classic "
        "motifs include knight-promotion forks and "
        "bishop/rook promotion to dodge stalemate."));
    connect(up_filter_, &QCheckBox::toggled,
            this, [this](bool on) { proxy_->set_up_only(on); });
    filter_row->addWidget(up_filter_);

    kf_filter_ = new QCheckBox(tr("knight fork"), this);
    kf_filter_->setToolTip(tr(
        "Show only games with at least one knight fork — a "
        "knight move that gives check AND simultaneously "
        "attacks an opponent queen or rook. The classic "
        "royal-fork / family-check motif."));
    connect(kf_filter_, &QCheckBox::toggled,
            this, [this](bool on) { proxy_->set_kf_only(on); });
    filter_row->addWidget(kf_filter_);

    sac_filter_ = new QCheckBox(tr("sacrifice"), this);
    sac_filter_->setToolTip(tr(
        "Show only games where one side lost ≥ 3 cp worth of "
        "material over two consecutive plies — a sacrifice "
        "(sound or otherwise). The Sac column shows the "
        "biggest single-event loss (Q/R/m) and how much of "
        "it was won back within a 10-ply window."));
    connect(sac_filter_, &QCheckBox::toggled,
            this, [this](bool on) { proxy_->set_sac_only(on); });
    filter_row->addWidget(sac_filter_);

    sac_won_filter_ = new QCheckBox(tr("sac won"), this);
    sac_won_filter_->setToolTip(tr(
        "Show only games where the side that sacrificed also "
        "won the game. A sacrifice that the sacrificer then "
        "converted — the sharpest filter for classical "
        "brilliancies. Drops unrecovered sacrifices in losses "
        "(plain blunders) and draws."));
    connect(sac_won_filter_, &QCheckBox::toggled,
            this, [this](bool on) { proxy_->set_sac_won_only(on); });
    filter_row->addWidget(sac_won_filter_);

    layout->addLayout(filter_row);

    model_ = new GameListModel(this);
    proxy_ = new GameFilterProxy(this);
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

bool GameListView::eventFilter(QObject* obj, QEvent* e) {
    if (e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape) {
            if (obj == name_filter_
                && !name_filter_->text().isEmpty()) {
                name_filter_->clear();
                return true;
            }
            if (obj == year_filter_->lineEdit()
                && !year_filter_->currentText().isEmpty()) {
                year_filter_->setCurrentText(QString{});
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, e);
}

bool GameListView::load(const QString& pgn_path) {
    source_path_ = pgn_path;
    pgn_bytes_.clear();
    games_.clear();
    last_chosen_row_ = -1;
    model_->set_games(nullptr);

    QFile f(pgn_path);
    if (!f.open(QIODevice::ReadOnly)) {
        status_->setText(tr("Could not open %1 for reading.").arg(pgn_path));
        return false;
    }
    const QByteArray bytes = f.readAll();
    pgn_bytes_.assign(bytes.constData(),
                      bytes.constData() + bytes.size());

    // Index sidecar: <PGN>.idx.json next to the PGN.
    // Reuse when mtime matches; otherwise rebuild with a
    // progress dialog and persist.
    const QFileInfo info(pgn_path);
    const std::int64_t pgn_mtime =
        info.lastModified().toSecsSinceEpoch();
    const QString idx_path =
        info.absolutePath() + QDir::separator()
        + info.completeBaseName() + QStringLiteral(".idx.json");

    bool reused = false;
    if (QFileInfo::exists(idx_path)) {
        auto loaded = load_index(idx_path.toStdString());
        if (loaded.has_value() && loaded->pgn_mtime == pgn_mtime) {
            games_ = std::move(loaded->games);
            reused = true;
        }
    }

    if (!reused) {
        QProgressDialog dlg(tr("Indexing %1 …").arg(info.fileName()),
                            tr("Cancel"),
                            /*min=*/0, /*max=*/0, this);
        dlg.setWindowModality(Qt::WindowModal);
        dlg.setMinimumDuration(200);
        std::atomic<bool> cancel{false};

        GameIndex idx = build_index(
            pgn_bytes_,
            pgn_mtime,
            [&](std::size_t done, std::size_t total) {
                // First progress tick: fix the range so the bar
                // reflects real progress instead of the busy
                // marquee.
                if (dlg.maximum() == 0) {
                    dlg.setMaximum(static_cast<int>(total));
                }
                dlg.setValue(static_cast<int>(done));
                if (dlg.wasCanceled()) cancel.store(true);
                QApplication::processEvents();
            },
            cancel);

        // Persist even partial indexes on cancel — next open
        // will re-detect mtime match and skip re-work for the
        // prefix that did complete.
        (void)save_index(idx_path.toStdString(), idx);
        games_ = std::move(idx.games);
    }

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

    // Filters start empty after a new load — clear the name
    // and seed the year combo from every indexed game.
    name_filter_->blockSignals(true);
    name_filter_->clear();
    name_filter_->blockSignals(false);
    year_filter_->blockSignals(true);
    year_filter_->setCurrentText(QString{});
    year_filter_->blockSignals(false);
    proxy_->set_name(QString{});
    proxy_->set_year(QString{});
    refresh_year_combo();

    status_->setText(tr(
        "%1 — %2 game(s) indexed. Double-click a row to open.")
            .arg(QFileInfo(pgn_path).fileName())
            .arg(games_.size()));
    return true;
}

void GameListView::on_name_filter_changed() {
    const QString s = name_filter_->text().trimmed();
    proxy_->set_name(s);
    // Years depend on the name filter: the combo should only
    // offer years that actually exist among name-matching
    // games, so narrowing a name makes the year choice set
    // shrink accordingly.
    refresh_year_combo();
}

void GameListView::on_year_filter_changed() {
    proxy_->set_year(year_filter_->currentText().trimmed());
}

void GameListView::refresh_year_combo() {
    const QString current = year_filter_->currentText();
    const QString name = name_filter_->text().trimmed();

    QSet<QString> years;
    for (const GameRecord& rec : games_) {
        const PgnGameHeader& g = rec.header;
        if (!name.isEmpty()) {
            const QString w = QString::fromStdString(g.white);
            const QString b = QString::fromStdString(g.black);
            if (!w.contains(name, Qt::CaseInsensitive)
                && !b.contains(name, Qt::CaseInsensitive)) {
                continue;
            }
        }
        const QString d = QString::fromStdString(g.date);
        const QString y = d.left(4);
        // Skip "????", "", and anything that doesn't look like
        // a plausible year prefix.
        if (y.size() == 4 && y[0].isDigit()) {
            years.insert(y);
        }
    }
    QStringList sorted(years.begin(), years.end());
    std::sort(sorted.begin(), sorted.end());

    year_filter_->blockSignals(true);
    year_filter_->clear();
    year_filter_->addItem(QString{}); // "(any)"
    year_filter_->addItems(sorted);
    // Restore whatever the user had typed. If it no longer
    // matches anything the combo just shows it in the line
    // edit without a list selection — that keeps the live
    // filter honest (the user sees 0 rows and knows why).
    year_filter_->setCurrentText(current);
    year_filter_->blockSignals(false);
}

void GameListView::select_row(std::size_t source_row_) {
    if (source_row_ >= games_.size()) return;
    last_chosen_row_ = static_cast<int>(source_row_);
    reselect_last();
}

void GameListView::reselect_last() {
    if (last_chosen_row_ < 0) return;
    if (static_cast<std::size_t>(last_chosen_row_) >= games_.size()) return;
    const QModelIndex src = model_->index(last_chosen_row_, 0);
    const QModelIndex view = proxy_->mapFromSource(src);
    if (!view.isValid()) return; // filtered out
    table_->selectRow(view.row());
    table_->scrollTo(view, QAbstractItemView::PositionAtCenter);
    table_->setFocus();
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
    last_chosen_row_ = row;

    const GameRecord& rec = games_[r];
    const PgnGameHeader& g = rec.header;
    if (g.offset + g.length > pgn_bytes_.size()) return;

    const std::string_view slice(
        pgn_bytes_.data() + g.offset, g.length);
    const QString pgn_text = QString::fromUtf8(
        slice.data(), static_cast<qsizetype>(slice.size()));
    const QString label = QStringLiteral("%1 — %2, %3")
        .arg(QString::fromStdString(g.white))
        .arg(QString::fromStdString(g.black))
        .arg(QString::fromStdString(g.date));

    // Column-aware jump: the cell the user activated decides
    // where we land in the move list. -1 = default (viewer
    // lands at end of game).
    int target_ply = -1;
    const int proxy_col = idx.column();
    switch (proxy_col) {
        case GameListModel::ColSac: {
            // Sacrificing move = ply of the biggest-raw-loss sac.
            if (!rec.material_sacs.empty()) {
                const MaterialSac* best = &rec.material_sacs.front();
                for (const auto& s : rec.material_sacs) {
                    if (s.raw_loss_cp > best->raw_loss_cp) best = &s;
                }
                target_ply = best->ply;
            }
            break;
        }
        case GameListModel::ColKF:
            if (!rec.knight_fork_plies.empty()) {
                target_ply = rec.knight_fork_plies.front();
            }
            break;
        case GameListModel::ColUP:
            if (!rec.underpromotions.empty()) {
                target_ply = rec.underpromotions.front().ply;
            }
            break;
        default:
            break; // leave -1
    }

    emit game_chosen(pgn_text, label, target_ply);
}

} // namespace chesserazade::analyzer
