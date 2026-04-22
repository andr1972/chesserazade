/// Fetch dialog — pick a PGN Mentor player archive, download
/// (unless cached), unzip, and hand the resulting `.pgn` path
/// back to the MainWindow.
///
/// Cached entries render with a distinct icon so the user sees
/// at a glance which archives are already on disk and can be
/// opened without hitting the network.
#pragma once

#include "pgnmentor_index.hpp"

#include <QDialog>
#include <QString>

#include <vector>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace chesserazade::analyzer {

class FetchDialog final : public QDialog {
    Q_OBJECT
public:
    explicit FetchDialog(QWidget* parent = nullptr);

    /// The PGN file that the user selected at OK-time. Empty
    /// until a successful download + unzip. Usable after
    /// `exec()` returns `QDialog::Accepted`.
    [[nodiscard]] QString selected_pgn_path() const
        { return selected_pgn_; }

signals:
    /// Convenience for callers that wire `exec()` asynchronously.
    /// Carries the same value as `selected_pgn_path()`.
    void pgn_ready(const QString& pgn_path);

private:
    void on_selection_changed();
    void on_open_clicked();
    void rebuild_list();
    [[nodiscard]] QString cache_dir() const;
    [[nodiscard]] QString pgn_path_for(const PgnMentorEntry& e) const;
    [[nodiscard]] QString zip_path_for(const PgnMentorEntry& e) const;

    std::vector<PgnMentorEntry> entries_;
    QListWidget* list_     = nullptr;
    QLabel*      status_   = nullptr;
    QPushButton* open_btn_ = nullptr;
    QString      selected_pgn_;
};

} // namespace chesserazade::analyzer
