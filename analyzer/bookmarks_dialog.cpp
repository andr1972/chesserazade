#include "bookmarks_dialog.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace chesserazade::analyzer {

namespace {

constexpr int RoleBookmarkIdx = Qt::UserRole + 1;

[[nodiscard]] QString leaf_label(const Bookmark& b) {
    QString s = b.label.isEmpty()
        ? QStringLiteral("(no label)")
        : b.label;
    return s + QStringLiteral("  —  ") + ply_to_notation(b.ply);
}

[[nodiscard]] QString folder_header(const QString& name, int count) {
    const QString display = name.isEmpty()
        ? QStringLiteral("(no folder)")
        : name;
    return QStringLiteral("%1  [%2]").arg(display).arg(count);
}

} // namespace

BookmarksDialog::BookmarksDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Bookmarks"));
    resize(720, 520);

    auto* layout = new QVBoxLayout(this);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    tree_ = new QTreeWidget(splitter);
    tree_->setHeaderLabels({tr("Folder / bookmark")});
    tree_->setRootIsDecorated(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(tree_, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem*, QTreeWidgetItem*) {
                on_selection_changed();
            });
    connect(tree_, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem*, int) {
                if (open_btn_->isEnabled()) on_open_clicked();
            });
    splitter->addWidget(tree_);

    auto* detail_pane = new QWidget(splitter);
    auto* detail_lay = new QVBoxLayout(detail_pane);
    detail_lay->setContentsMargins(0, 0, 0, 0);
    auto* detail_label = new QLabel(tr("Comment"), detail_pane);
    detail_lay->addWidget(detail_label);
    detail_ = new QPlainTextEdit(detail_pane);
    detail_->setReadOnly(true);
    detail_->setPlaceholderText(tr("No comment."));
    detail_lay->addWidget(detail_, /*stretch=*/1);
    splitter->addWidget(detail_pane);

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, /*stretch=*/1);

    auto* buttons = new QDialogButtonBox(this);
    open_btn_ = buttons->addButton(tr("&Open"),
                                   QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    open_btn_->setEnabled(false);
    connect(open_btn_, &QPushButton::clicked,
            this, &BookmarksDialog::on_open_clicked);
    connect(buttons->button(QDialogButtonBox::Close),
            &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttons);

    all_ = load_bookmarks().value_or(std::vector<Bookmark>{});
    rebuild_tree();
}

void BookmarksDialog::rebuild_tree() {
    tree_->clear();

    // Partition indices by folder. Empty-folder bookmarks
    // collapse into a synthetic "(no folder)" bucket at the top.
    QStringList folders = folders_in_use(all_);
    // Track how many bookmarks have no folder so the synthetic
    // bucket surfaces only when it is non-empty.
    int unfiled = 0;
    for (const Bookmark& b : all_) if (b.folder.isEmpty()) ++unfiled;
    if (unfiled > 0) folders.prepend(QString{});

    auto make_group = [&](const QString& name, int count) {
        auto* top = new QTreeWidgetItem(tree_);
        top->setText(0, folder_header(name, count));
        top->setFlags(Qt::ItemIsEnabled);
        QFont f = top->font(0);
        f.setBold(true);
        top->setFont(0, f);
        return top;
    };

    for (const QString& folder : folders) {
        // Collect + stable-sort by created_ms desc so newest
        // bookmarks surface at the top of each folder.
        std::vector<std::size_t> idxs;
        for (std::size_t i = 0; i < all_.size(); ++i) {
            if (all_[i].folder == folder) idxs.push_back(i);
        }
        std::sort(idxs.begin(), idxs.end(),
                  [this](std::size_t a, std::size_t b) {
                      return all_[a].created_ms > all_[b].created_ms;
                  });
        auto* group = make_group(folder, static_cast<int>(idxs.size()));
        for (std::size_t i : idxs) {
            auto* leaf = new QTreeWidgetItem(group);
            leaf->setText(0, leaf_label(all_[i]));
            leaf->setData(0, RoleBookmarkIdx,
                          static_cast<int>(i));
            leaf->setToolTip(0, QStringLiteral("%1 vs %2 · %3 · %4")
                .arg(all_[i].white.isEmpty() ? QStringLiteral("?")
                                             : all_[i].white)
                .arg(all_[i].black.isEmpty() ? QStringLiteral("?")
                                             : all_[i].black)
                .arg(all_[i].date)
                .arg(all_[i].zip));
        }
        group->setExpanded(true);
    }

    if (all_.empty()) {
        auto* placeholder = new QTreeWidgetItem(tree_);
        placeholder->setText(0, tr(
            "No bookmarks yet. Ctrl+D in the game view or "
            "solve panel saves one."));
        placeholder->setFlags(Qt::ItemIsEnabled);
    }
}

void BookmarksDialog::on_selection_changed() {
    QTreeWidgetItem* item = tree_->currentItem();
    const QVariant v = (item != nullptr)
        ? item->data(0, RoleBookmarkIdx) : QVariant{};
    if (!v.isValid()) {
        open_btn_->setEnabled(false);
        detail_->clear();
        return;
    }
    const int idx = v.toInt();
    if (idx < 0 || static_cast<std::size_t>(idx) >= all_.size()) {
        open_btn_->setEnabled(false);
        detail_->clear();
        return;
    }
    open_btn_->setEnabled(true);
    detail_->setPlainText(all_[static_cast<std::size_t>(idx)].comment);
}

void BookmarksDialog::on_open_clicked() {
    QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) return;
    const QVariant v = item->data(0, RoleBookmarkIdx);
    if (!v.isValid()) return;
    const int idx = v.toInt();
    if (idx < 0 || static_cast<std::size_t>(idx) >= all_.size()) return;
    chosen_ = all_[static_cast<std::size_t>(idx)];
    accept();
}

} // namespace chesserazade::analyzer
