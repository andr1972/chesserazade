#include "solve_panel.hpp"

#include "board_widget.hpp"
#include "filter_dialog.hpp"
#include "search_tree_model.hpp"

#include <chesserazade/fen.hpp>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QMessageBox>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

namespace chesserazade::analyzer {

SolvePanel::SolvePanel(QWidget* parent)
    : QWidget(parent),
      position_(*Board8x8Mailbox::from_fen(STARTING_POSITION_FEN)),
      displayed_board_(*Board8x8Mailbox::from_fen(STARTING_POSITION_FEN)) {
    setFocusPolicy(Qt::StrongFocus);

    // Create the tree model up-front so later widget wiring
    // (the Lazy checkbox, the tree view itself) can connect
    // into it without hitting a nullptr receiver.
    tree_model_ = new SearchTreeModel(this);

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

    lazy_check_ = new QCheckBox(tr("Lazy"), right);
    lazy_check_->setChecked(true);
    lazy_check_->setToolTip(tr(
        "When enabled, arrows on cap-bounded leaves trigger a "
        "sub-search and graft the resulting subtree in place. "
        "When disabled, the arrows still render (so you can "
        "inspect which nodes are expandable) but clicking them "
        "does nothing — useful as a debug view."));
    connect(lazy_check_, &QCheckBox::toggled,
            tree_model_, &SearchTreeModel::set_lazy_enabled);
    cap_row->addWidget(lazy_check_);

    ab_check_ = new QCheckBox(tr("α-β"), right);
    ab_check_->setChecked(true);
    ab_check_->setToolTip(tr(
        "Alpha-beta pruning. Normally on — the engine skips "
        "branches it can prove are worse than an already-found "
        "line. Turn off to see what the raw minimax tree looks "
        "like; use tiny max-ply only (no cutoffs means the tree "
        "explodes fast)."));
    cap_row->addWidget(ab_check_);

    qs_check_ = new QCheckBox(tr("quiesce"), right);
    qs_check_->setChecked(true);
    qs_check_->setToolTip(tr(
        "Quiescence search — at the horizon keep following "
        "captures until the position is quiet, then evaluate. "
        "Turn off to see the raw static eval at the horizon "
        "(horizon effect visible: mid-trade positions score "
        "as if the trade never happened)."));
    cap_row->addWidget(qs_check_);

    rfw_check_ = new QCheckBox(tr("root exact"), right);
    rfw_check_->setChecked(false);
    rfw_check_->setToolTip(tr(
        "Search each root move with a full α-β window so "
        "every score in the top of the tree is exact, not a "
        "bound dragged toward α by earlier siblings. Deeper "
        "nodes still prune normally. Costs 2–3× more work at "
        "the root — useful for analysis, off for speed."));
    cap_row->addWidget(rfw_check_);

    tt_check_ = new QCheckBox(tr("TT"), right);
    tt_check_->setChecked(true);
    tt_check_->setToolTip(tr(
        "Transposition table — caches per-position scores "
        "keyed by Zobrist hash. Speeds up every search above "
        "depth 2: transpositions skipped directly, and the "
        "previous iteration's best move becomes the TT move "
        "that tightens α-β cutoffs. Turn off to see the cost "
        "of pure search without caching."));
    cap_row->addWidget(tt_check_);

    incr_eval_check_ = new QCheckBox(tr("incr eval"), right);
    incr_eval_check_->setChecked(false);
    incr_eval_check_->setToolTip(tr(
        "Use the board's O(1) incrementally-maintained eval "
        "(material + PST running sum) instead of the full "
        "64-square scan. Identical results; purely a speed "
        "toggle to measure the saving."));
    cap_row->addWidget(incr_eval_check_);

    bitboard_check_ = new QCheckBox(tr("bitboard"), right);
    bitboard_check_->setChecked(false);
    bitboard_check_->setToolTip(tr(
        "Run the search on BoardBitboard (magic / PEXT slider "
        "attacks, popcount-based move generation) instead of "
        "the default mailbox. Move generation is 3–5× faster "
        "in perft terms; in search the total speedup is "
        "typically 2× because eval, TT and ordering share the "
        "cost."));
    cap_row->addWidget(bitboard_check_);

    cap_row->addStretch(1);
    rlay->addLayout(cap_row);

    // -- Run / Filter / Back ----------------------------------------
    auto* btn_row = new QHBoxLayout;
    run_btn_ = new QPushButton(tr("&Run"), right);
    auto* filter_btn = new QPushButton(tr("&Filter…"), right);
    back_btn_ = new QPushButton(tr("&Back"), right);
    progress_label_ = new QLabel(right);
    progress_label_->setMinimumWidth(160);
    connect(run_btn_,  &QPushButton::clicked,
            this, &SolvePanel::on_run_or_break_clicked);
    connect(filter_btn, &QPushButton::clicked,
            this, &SolvePanel::on_filter_clicked);
    connect(back_btn_, &QPushButton::clicked,
            this, &SolvePanel::back_requested);
    auto* copy_fen_btn = new QPushButton(tr("Copy &FEN"), right);
    auto* set_fen_btn  = new QPushButton(tr("Set FEN…"), right);
    copy_fen_btn->setToolTip(tr(
        "Copy the FEN of the board currently shown (sentinel "
        "or a tree-node descent) to the clipboard."));
    set_fen_btn->setToolTip(tr(
        "Replace the solve's base position with a FEN string."));
    connect(copy_fen_btn, &QPushButton::clicked,
            this, &SolvePanel::on_copy_fen_clicked);
    connect(set_fen_btn, &QPushButton::clicked,
            this, &SolvePanel::on_set_fen_clicked);
    btn_row->addWidget(run_btn_);
    btn_row->addWidget(filter_btn);
    btn_row->addWidget(back_btn_);
    btn_row->addWidget(copy_fen_btn);
    btn_row->addWidget(set_fen_btn);
    btn_row->addWidget(progress_label_);
    btn_row->addStretch(1);
    rlay->addLayout(btn_row);

    // Refreshes the "progress_label_" from the atomics written
    // by the search thread. 100 ms → 10 Hz updates.
    progress_timer_ = new QTimer(this);
    progress_timer_->setInterval(100);
    connect(progress_timer_, &QTimer::timeout,
            this, &SolvePanel::on_progress_tick);

    // -- Tree + info log (vertical split; tree on top) --------------
    auto* tree_log = new QSplitter(Qt::Vertical, right);

    auto* tree_group = new QWidget(tree_log);
    auto* tree_lay = new QVBoxLayout(tree_group);
    tree_lay->setContentsMargins(0, 0, 0, 0);
    auto* tree_label = new QLabel(tr("Search tree"), tree_group);
    QFont tl_font = tree_label->font();
    tl_font.setBold(true);
    tree_label->setFont(tl_font);
    tree_lay->addWidget(tree_label);

    tree_view_ = new QTreeView(tree_group);
    tree_view_->setModel(tree_model_);
    tree_view_->setUniformRowHeights(true);
    tree_view_->setAllColumnsShowFocus(true);
    tree_view_->setAlternatingRowColors(true);
    // Keep rows in model-insertion order. That is the search
    // order produced by the move-ordering heuristic (TT move
    // first, MVV-LVA captures, killers, …), so the top-level
    // rows reorder between ID iterations when the best move
    // changes — which is exactly what the user wants to see.
    tree_view_->setSortingEnabled(false);
    tree_view_->setRootIsDecorated(true);
    tree_view_->setItemsExpandable(true);
    tree_view_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_view_->header()->setDefaultSectionSize(80);
    // The sibling-index column is only a small integer — keep
    // it narrow so the Move / Score columns get the space.
    tree_view_->setColumnWidth(SearchTreeModel::ColId, 40);
    // Draw the expand/collapse arrow and the indent guide in
    // the Move column rather than the default column 0 (#).
    // With a 40-px # column the tree indicator was physically
    // there but visually swallowed by a two-digit row number;
    // anchoring to Move puts the hierarchy exactly where the
    // reader expects it, next to each move's SAN.
    tree_view_->setTreePosition(SearchTreeModel::ColMove);
    // Keyboard nav (arrow keys) changes the current selection;
    // we want the board to follow that, not only mouse clicks.
    // Qt happily drops the signal's trailing `previous` arg when
    // the slot has fewer parameters, so we can wire
    // currentChanged straight into on_tree_row_clicked.
    connect(tree_view_->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this, &SolvePanel::on_tree_row_clicked);
    connect(tree_view_, &QTreeView::clicked,
            this, &SolvePanel::on_tree_row_clicked);
    // Queued so the model mutation happens *after* fetchMore
    // returns — Qt is unhappy when rows are inserted while
    // the view is still inside its own fetchMore call.
    connect(tree_model_, &SearchTreeModel::expansion_requested,
            this, &SolvePanel::on_expansion_requested,
            Qt::QueuedConnection);
    tree_lay->addWidget(tree_view_, /*stretch=*/1);
    tree_log->addWidget(tree_group);

    auto* log_group = new QWidget(tree_log);
    auto* log_lay = new QVBoxLayout(log_group);
    log_lay->setContentsMargins(0, 0, 0, 0);
    auto* log_label = new QLabel(tr("Progress log"), log_group);
    log_label->setFont(tl_font);
    log_lay->addWidget(log_label);

    log_ = new QPlainTextEdit(log_group);
    log_->setReadOnly(true);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log_lay->addWidget(log_, /*stretch=*/1);
    tree_log->addWidget(log_group);

    tree_log->setStretchFactor(0, 3);  // tree bigger
    tree_log->setStretchFactor(1, 1);

    rlay->addWidget(tree_log, /*stretch=*/1);

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
    displayed_board_ = board;
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
    b.disable_alpha_beta = !ab_check_->isChecked();
    b.disable_quiescence = !qs_check_->isChecked();
    b.root_full_window   =  rfw_check_->isChecked();
    b.use_tt             =  tt_check_->isChecked();
    b.use_incremental_eval = incr_eval_check_->isChecked();
    b.use_bitboard         = bitboard_check_->isChecked();
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
    // run_btn_ stays enabled during a search — it doubles as
    // the Break button — so its label is swapped by the
    // run / cancel / finished handlers, not here.
    rb_depth_->setEnabled(!running);
    rb_time_->setEnabled(!running);
    rb_nodes_->setEnabled(!running);
    depth_spin_->setEnabled(!running);
    time_spin_->setEnabled(!running);
    nodes_spin_->setEnabled(!running);
    nodes_mult_->setEnabled(!running);
    back_btn_->setEnabled(!running);
}

void SolvePanel::on_run_or_break_clicked() {
    if (thread_ != nullptr) {
        // A search is in flight — the button is labelled
        // "Break"; clicking it sets the atomic the search
        // polls on its stop cadence. The button remains
        // enabled but read-only till the worker actually
        // exits (usually within a few milliseconds).
        cancel_.store(true, std::memory_order_relaxed);
        run_btn_->setEnabled(false);
        append_log(tr("Break requested…"));
        return;
    }
    on_run_clicked();
}

void SolvePanel::on_progress_tick() {
    const auto elapsed =
        std::chrono::steady_clock::now() - search_start_;
    const double s =
        std::chrono::duration<double>(elapsed).count();
    const double mn =
        static_cast<double>(progress_nodes_.load(
            std::memory_order_relaxed)) / 1.0e6;
    progress_label_->setText(
        QStringLiteral("%1 s   %2 M nodes")
            .arg(s, 0, 'f', 1)
            .arg(mn, 0, 'f', 2));
}

void SolvePanel::on_run_clicked() {
    if (thread_ != nullptr) return; // already running

    clear_log();
    append_log(tr("Searching…"));
    set_running(true);

    // Reset the atomics before arming the worker; start the
    // progress timer so the label ticks from 0.
    cancel_.store(false, std::memory_order_relaxed);
    progress_nodes_.store(0, std::memory_order_relaxed);
    search_start_ = std::chrono::steady_clock::now();
    progress_label_->clear();
    progress_timer_->start();

    // Run button now means "Break" while the search runs.
    run_btn_->setText(tr("&Break"));
    run_btn_->setEnabled(true);

    thread_ = new QThread(this);
    worker_ = new SolveWorker(position_, current_budget(),
                              &cancel_, &progress_nodes_);
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
            this, &SolvePanel::on_worker_thread_destroyed);

    thread_->start();
}

void SolvePanel::on_worker_thread_destroyed() {
    thread_ = nullptr;
    worker_ = nullptr;
    set_running(false);
    // Put the button back into "Run" mode for the next search.
    run_btn_->setText(tr("&Run"));
    run_btn_->setEnabled(true);
    progress_timer_->stop();
    // Leave the progress_label_ showing the final numbers; the
    // next on_run_clicked will clear it.
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

    // Stop polling and write the exact final tallies into the
    // progress label. The timer's atomic reads lag by up to
    // ~2048 nodes (STOP_CHECK_MASK cadence); the `finished`
    // signal carries the true totals, so we render those.
    progress_timer_->stop();
    const double s  = static_cast<double>(elapsed_ms) / 1000.0;
    const double mn = static_cast<double>(nodes) / 1.0e6;
    progress_label_->setText(
        QStringLiteral("%1 s   %2 M nodes")
            .arg(s, 0, 'f', 1)
            .arg(mn, 0, 'f', 2));

    // Tree was already installed by the last
    // `iteration_tree_ready` signal from the deepest completed
    // iteration — nothing further to do here.
}

void SolvePanel::on_iteration_tree_ready(const SearchTree& incoming) {
    tree_ = incoming;
    // Detach first so the view drops any cached expand state
    // keyed on indices from the previous iteration, then
    // re-point.
    tree_model_->set_tree(nullptr);
    tree_model_->set_tree(&tree_);
    // Expand only the "/" sentinel so its ~40 children become
    // visible immediately; deeper subtrees stay collapsed so
    // the view does not drown in siblings.
    tree_view_->collapseAll();
    tree_view_->expandToDepth(0);
    tree_view_->resizeColumnToContents(SearchTreeModel::ColMove);
}

void SolvePanel::on_tree_row_clicked(const QModelIndex& idx) {
    if (!idx.isValid()) return;
    const auto moves = tree_model_->moves_to(idx);
    Board8x8Mailbox b = position_;
    for (const Move& m : moves) {
        b.make_move(m);
    }
    displayed_board_ = b;
    board_->set_position(b);
}

void SolvePanel::on_copy_fen_clicked() {
    const std::string fen = serialize_fen(displayed_board_);
    QGuiApplication::clipboard()->setText(QString::fromStdString(fen));
    // Short-lived toast via the log so the user has a
    // confirmation without an intrusive dialog.
    append_log(tr("FEN copied: %1").arg(QString::fromStdString(fen)));
}

void SolvePanel::on_set_fen_clicked() {
    bool ok = false;
    const QString fen = QInputDialog::getText(
        this, tr("Set solve position from FEN"),
        tr("FEN:"),
        QLineEdit::Normal,
        QString::fromStdString(serialize_fen(displayed_board_)),
        &ok);
    if (!ok || fen.isEmpty()) return;
    auto parsed = Board8x8Mailbox::from_fen(fen.toStdString());
    if (!parsed) {
        QMessageBox::warning(
            this, tr("Invalid FEN"),
            tr("Could not parse the FEN: %1")
                .arg(QString::fromStdString(parsed.error().message)));
        return;
    }
    set_position(*parsed, tr("FEN: %1").arg(fen));
}

void SolvePanel::on_filter_clicked() {
    const int cap = tree_cap_spin_->value();
    FilterDialog dlg(cap, tree_model_->filter(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    tree_model_->set_filter(dlg.state());
    // The model reset collapses the view; re-open the sentinel
    // so the filtered top-level is visible immediately.
    tree_view_->collapseAll();
    tree_view_->expandToDepth(0);
    tree_view_->resizeColumnToContents(SearchTreeModel::ColMove);
}

void SolvePanel::on_expansion_requested(int node_idx) {
    if (node_idx <= 0 || node_idx >= tree_.size()) return;
    const TreeNode& node = tree_.at(node_idx);
    if (!node.children.empty() || node.remaining_depth <= 0) return;

    // Reconstruct the board at this node by replaying the
    // ancestor chain onto the solve's base position.
    Board8x8Mailbox board = position_;
    {
        std::vector<Move> chain;
        int cur = node_idx;
        while (cur > 0) {
            chain.push_back(tree_.at(cur).move);
            cur = tree_.at(cur).parent;
        }
        std::reverse(chain.begin(), chain.end());
        for (const Move& m : chain) board.make_move(m);
    }

    // Respect the main search's overall max_ply: remaining_depth
    // is exactly `max_ply - current_ply` and caps how far the
    // sub-search may go from this node. Within that budget the
    // recorder cap (user-chosen, default 3) limits how many
    // plies of new nesting are recorded per click.
    SearchLimits lim;
    lim.max_depth = node.remaining_depth;
    lim.disable_alpha_beta = !ab_check_->isChecked();
    lim.disable_quiescence = !qs_check_->isChecked();
    lim.root_full_window   =  rfw_check_->isChecked();

    SearchTree sub;
    SearchTreeRecorder sub_rec(sub, tree_cap_spin_->value());

    (void)Search::find_best(board, lim, /*tt=*/nullptr, &sub_rec,
                            node.alpha, node.beta);
    sub.finalize_san(board);
    sub.mark_best_subtrees();

    // Splice the new subtree in-place under `node_idx`; the
    // model notifies the view which draws the new rows.
    tree_model_->insert_subtree(node_idx, sub);

    // insert_subtree currently does a full beginResetModel,
    // so every expanded row got collapsed. Walk back up to
    // the sentinel and re-expand each ancestor so the grafted
    // subtree is visible without a second click.
    std::vector<int> ancestors;
    for (int cur = node_idx; cur >= 0;
         cur = (cur == 0 ? -1 : tree_.at(cur).parent)) {
        ancestors.push_back(cur);
    }
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        const QModelIndex ix = tree_model_->index_for_node(*it);
        if (ix.isValid()) tree_view_->expand(ix);
    }

    // Deeper nesting means more indent ahead of the SAN; let
    // the Move column grow so the expand arrow stays visible
    // at every level instead of getting clipped on the right.
    tree_view_->resizeColumnToContents(SearchTreeModel::ColMove);
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
