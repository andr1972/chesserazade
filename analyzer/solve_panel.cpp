#include "solve_panel.hpp"

#include "board_widget.hpp"
#include "search_tree_model.hpp"

#include <chesserazade/fen.hpp>

#include <QButtonGroup>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QSplitter>
#include <QThread>
#include <QTreeView>
#include <QVBoxLayout>

namespace chesserazade::analyzer {

SolvePanel::SolvePanel(QWidget* parent)
    : QWidget(parent),
      position_(*Board8x8Mailbox::from_fen(STARTING_POSITION_FEN)) {
    setFocusPolicy(Qt::StrongFocus);

    auto* outer = new QVBoxLayout(this);

    header_ = new QLabel(this);
    QFont hfont = header_->font();
    hfont.setBold(true);
    header_->setFont(hfont);
    outer->addWidget(header_);

    auto* split = new QSplitter(Qt::Horizontal, this);

    board_ = new BoardWidget(split);
    board_->setMinimumSize(280, 280);
    split->addWidget(board_);

    auto* right = new QWidget(split);
    auto* rlay = new QVBoxLayout(right);

    // -- Budget group -----------------------------------------------
    auto* budget_box = new QGroupBox(tr("Budget"), right);
    auto* budget_lay = new QVBoxLayout(budget_box);
    kind_group_ = new QButtonGroup(this);

    auto add_row = [&](QRadioButton*& rb, const QString& label,
                       QWidget* control,
                       QWidget* trailing = nullptr) {
        auto* row = new QHBoxLayout;
        rb = new QRadioButton(label, budget_box);
        kind_group_->addButton(rb);
        row->addWidget(rb);
        row->addWidget(control, /*stretch=*/1);
        if (trailing != nullptr) row->addWidget(trailing);
        budget_lay->addLayout(row);
    };

    depth_spin_ = new QSpinBox(budget_box);
    depth_spin_->setRange(1, Search::MAX_DEPTH);
    depth_spin_->setValue(6);
    add_row(rb_depth_, tr("Max ply"), depth_spin_);

    time_spin_ = new QSpinBox(budget_box);
    time_spin_->setRange(1, 3600);
    time_spin_->setSuffix(tr(" s"));
    time_spin_->setValue(5);
    add_row(rb_time_, tr("Time"), time_spin_);

    nodes_spin_ = new QSpinBox(budget_box);
    nodes_spin_->setRange(1, 1'000'000);
    nodes_spin_->setValue(1000);
    nodes_mult_ = new QComboBox(budget_box);
    nodes_mult_->addItem(tr("× 1 000"),     1'000);
    nodes_mult_->addItem(tr("× 1 000 000"), 1'000'000);
    nodes_mult_->setCurrentIndex(0);
    add_row(rb_nodes_, tr("Nodes"), nodes_spin_, nodes_mult_);

    rb_time_->setChecked(true);
    rlay->addWidget(budget_box);

    auto* cap_row = new QHBoxLayout;
    cap_row->addWidget(new QLabel(tr("Tree ply cap:"), right));
    tree_cap_spin_ = new QSpinBox(right);
    tree_cap_spin_->setRange(1, 6);
    tree_cap_spin_->setValue(3);
    tree_cap_spin_->setToolTip(tr(
        "Only moves at this depth or shallower are recorded "
        "into the tree view. Deeper search still runs; its "
        "effects bubble up as capture / check totals."));
    cap_row->addWidget(tree_cap_spin_);
    cap_row->addStretch(1);
    rlay->addLayout(cap_row);

    // -- Run / Back -------------------------------------------------
    auto* btn_row = new QHBoxLayout;
    run_btn_ = new QPushButton(tr("&Run"), right);
    back_btn_ = new QPushButton(tr("&Back"), right);
    connect(run_btn_,  &QPushButton::clicked,
            this, &SolvePanel::on_run_clicked);
    connect(back_btn_, &QPushButton::clicked,
            this, &SolvePanel::back_requested);
    btn_row->addWidget(run_btn_);
    btn_row->addWidget(back_btn_);
    btn_row->addStretch(1);
    rlay->addLayout(btn_row);

    // -- Info log + tree (vertical split) ---------------------------
    auto* log_tree = new QSplitter(Qt::Vertical, right);

    log_ = new QPlainTextEdit(log_tree);
    log_->setReadOnly(true);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log_tree->addWidget(log_);

    tree_model_ = new SearchTreeModel(this);
    tree_view_ = new QTreeView(log_tree);
    tree_view_->setModel(tree_model_);
    tree_view_->setUniformRowHeights(true);
    tree_view_->setAllColumnsShowFocus(true);
    tree_view_->setAlternatingRowColors(true);
    tree_view_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_view_->header()->setDefaultSectionSize(80);
    connect(tree_view_, &QTreeView::clicked,
            this, &SolvePanel::on_tree_row_clicked);
    log_tree->addWidget(tree_view_);
    log_tree->setStretchFactor(0, 1);
    log_tree->setStretchFactor(1, 3);

    rlay->addWidget(log_tree, /*stretch=*/1);

    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    outer->addWidget(split, /*stretch=*/1);
}

SolvePanel::~SolvePanel() {
    if (thread_ != nullptr) {
        thread_->quit();
        thread_->wait();
    }
}

void SolvePanel::set_position(const Board8x8Mailbox& board,
                              const QString& header_label) {
    position_ = board;
    board_->set_position(board);
    header_->setText(tr("Solve from: %1").arg(header_label));
    clear_log();
    tree_.reset();
    tree_model_->set_tree(nullptr);
    setFocus();
}

SolveBudget SolvePanel::current_budget() const {
    SolveBudget b;
    b.tree_cap = tree_cap_spin_->value();
    if (rb_depth_->isChecked()) {
        b.kind = SolveBudget::Kind::Depth;
        b.depth = depth_spin_->value();
    } else if (rb_time_->isChecked()) {
        b.kind = SolveBudget::Kind::TimeMs;
        b.time_ms = time_spin_->value() * 1000;
    } else {
        b.kind = SolveBudget::Kind::Nodes;
        const long long mult =
            nodes_mult_->currentData().toLongLong();
        b.nodes = static_cast<long long>(nodes_spin_->value()) * mult;
    }
    return b;
}

void SolvePanel::clear_log() {
    log_->clear();
}

void SolvePanel::append_log(const QString& line) {
    log_->appendPlainText(line);
}

void SolvePanel::set_running(bool running) {
    run_btn_->setEnabled(!running);
    rb_depth_->setEnabled(!running);
    rb_time_->setEnabled(!running);
    rb_nodes_->setEnabled(!running);
    depth_spin_->setEnabled(!running);
    time_spin_->setEnabled(!running);
    nodes_spin_->setEnabled(!running);
    nodes_mult_->setEnabled(!running);
    back_btn_->setEnabled(!running);
}

void SolvePanel::on_run_clicked() {
    if (thread_ != nullptr) return; // already running

    clear_log();
    append_log(tr("Searching…"));
    set_running(true);

    thread_ = new QThread(this);
    worker_ = new SolveWorker(position_, current_budget());
    worker_->moveToThread(thread_);

    connect(thread_, &QThread::started,
            worker_, &SolveWorker::start);
    connect(worker_, &SolveWorker::progress,
            this, &SolvePanel::on_progress);
    connect(worker_, &SolveWorker::iteration_tree_ready,
            this, &SolvePanel::on_iteration_tree_ready);
    connect(worker_, &SolveWorker::finished,
            this, &SolvePanel::on_finished);
    connect(worker_, &SolveWorker::finished,
            thread_, &QThread::quit);
    connect(thread_, &QThread::finished,
            worker_, &QObject::deleteLater);
    connect(thread_, &QThread::finished,
            thread_, &QObject::deleteLater);
    connect(thread_, &QThread::destroyed,
            this, [this]() {
                thread_ = nullptr;
                worker_ = nullptr;
                set_running(false);
            });

    thread_->start();
}

void SolvePanel::on_progress(int depth, int score_cp,
                             const QString& pv_uci,
                             int captures_white, int captures_black,
                             int checks_white,   int checks_black) {
    const QString score = Search::is_mate_score(score_cp)
        ? QStringLiteral("mate %1").arg(Search::plies_to_mate(score_cp))
        : QStringLiteral("cp %1").arg(score_cp);

    append_log(QStringLiteral(
        "depth %1  score %2  capt W/B %3/%4  chk W/B %5/%6  pv %7")
        .arg(depth, 2)
        .arg(score, -10)
        .arg(captures_white)
        .arg(captures_black)
        .arg(checks_white)
        .arg(checks_black)
        .arg(pv_uci));
}

void SolvePanel::on_finished(const QString& best_uci, int final_score,
                             int depth_reached, quint64 nodes,
                             qint64 elapsed_ms) {
    const QString score = Search::is_mate_score(final_score)
        ? QStringLiteral("mate %1")
              .arg(Search::plies_to_mate(final_score))
        : QStringLiteral("cp %1").arg(final_score);

    append_log(QStringLiteral(
        "— done: bestmove %1  score %2  depth %3  nodes %4  time %5 ms")
        .arg(best_uci).arg(score).arg(depth_reached)
        .arg(nodes).arg(elapsed_ms));

    // Tree was already installed by the last
    // `iteration_tree_ready` signal from the deepest completed
    // iteration — nothing further to do here.
}

void SolvePanel::on_iteration_tree_ready(const SearchTree& incoming) {
    tree_ = incoming;
    tree_model_->set_tree(&tree_);
    tree_view_->expandToDepth(1);
    tree_view_->resizeColumnToContents(SearchTreeModel::ColMove);
}

void SolvePanel::on_tree_row_clicked(const QModelIndex& idx) {
    if (!idx.isValid()) return;
    const auto moves = tree_model_->moves_to(idx);
    Board8x8Mailbox b = position_;
    for (const Move& m : moves) {
        b.make_move(m);
    }
    board_->set_position(b);
}

void SolvePanel::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape
        && (thread_ == nullptr)) {
        emit back_requested();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

} // namespace chesserazade::analyzer
