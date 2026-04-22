#include "filter_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace chesserazade::analyzer {

namespace {

QGroupBox* build_group(const QString& title, int cap,
                       const std::vector<bool>& initial,
                       std::vector<QCheckBox*>& out,
                       QWidget* parent) {
    auto* box = new QGroupBox(title, parent);
    auto* row = new QHBoxLayout(box);
    for (int i = 1; i <= cap; ++i) {
        auto* cb = new QCheckBox(QString::number(i), box);
        const std::size_t idx = static_cast<std::size_t>(i - 1);
        if (idx < initial.size() && initial[idx]) cb->setChecked(true);
        row->addWidget(cb);
        out.push_back(cb);
    }
    row->addStretch(1);
    return box;
}

} // namespace

FilterDialog::FilterDialog(int cap,
                           const SearchTreeModel::FilterState& initial,
                           QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Filter tree — significant events"));

    auto* layout = new QVBoxLayout(this);

    auto* header = new QLabel(
        tr("Show only branches whose path contains an event at "
           "at least one of the selected plies. Captures and "
           "checks combine with OR. Ply 1 is the first move "
           "after the solve position."), this);
    header->setWordWrap(true);
    layout->addWidget(header);

    layout->addWidget(build_group(tr("Captures on ply"), cap,
                                  initial.captures_on_ply,
                                  capture_boxes_, this));
    layout->addWidget(build_group(tr("Checks on ply"), cap,
                                  initial.checks_on_ply,
                                  check_boxes_, this));

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    layout->addWidget(buttons);
}

SearchTreeModel::FilterState FilterDialog::state() const {
    SearchTreeModel::FilterState s;
    s.captures_on_ply.reserve(capture_boxes_.size());
    for (auto* cb : capture_boxes_) {
        s.captures_on_ply.push_back(cb->isChecked());
    }
    s.checks_on_ply.reserve(check_boxes_.size());
    for (auto* cb : check_boxes_) {
        s.checks_on_ply.push_back(cb->isChecked());
    }
    return s;
}

} // namespace chesserazade::analyzer
