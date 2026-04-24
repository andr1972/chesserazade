/// Modal browser for saved bookmarks. Groups entries by folder,
/// shows a comment pane on the right, and returns the selected
/// bookmark to `MainWindow` so it can resolve it to an actual
/// game + ply and switch the view.
///
/// Scope: read-only in this commit. Edit / delete come later.
#pragma once

#include "bookmarks.hpp"

#include <QDialog>

#include <optional>

class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace chesserazade::analyzer {

class BookmarksDialog final : public QDialog {
    Q_OBJECT
public:
    explicit BookmarksDialog(QWidget* parent = nullptr);

    /// Bookmark the user picked. Populated at accept-time.
    [[nodiscard]] std::optional<Bookmark> chosen() const
        { return chosen_; }

private:
    void rebuild_tree();
    void on_selection_changed();
    void on_open_clicked();

    std::vector<Bookmark> all_;
    std::optional<Bookmark> chosen_;

    QTreeWidget*    tree_       = nullptr;
    QPlainTextEdit* detail_     = nullptr;
    QPushButton*    open_btn_   = nullptr;
};

} // namespace chesserazade::analyzer
