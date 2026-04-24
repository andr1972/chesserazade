#include "game_list_view.hpp"

#include "game_list_model.hpp"

#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QModelIndex>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

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
        return true;
    }

private:
    QString name_;
    QString year_;
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
    for (const PgnGameHeader& g : games_) {
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
