// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `FilterDialog` — lets the user prune the search-tree view
/// down to "significant events" only: branches that contain a
/// capture or a check at one of the selected plies.
///
/// Two sections of checkboxes, one per criterion, each listing
/// ply 1..cap. Within a criterion the checkboxes combine with
/// OR (a branch matches if any selected ply contains the
/// event). Captures and checks then combine with OR too, so a
/// branch is shown iff it contains *any* matching capture *or*
/// *any* matching check. An empty selection = filter off.
#pragma once

#include "search_tree_model.hpp"

#include <QDialog>

#include <vector>

class QCheckBox;

namespace chesserazade::analyzer {

class FilterDialog final : public QDialog {
    Q_OBJECT
public:
    /// `cap` is the current Tree ply cap; the dialog lays out
    /// one checkbox per ply from 1 to `cap`. `initial` pre-
    /// ticks boxes matching the currently-active filter.
    FilterDialog(int cap, const SearchTreeModel::FilterState& initial,
                 QWidget* parent = nullptr);

    /// Usable after the dialog closes with QDialog::Accepted.
    [[nodiscard]] SearchTreeModel::FilterState state() const;

private:
    std::vector<QCheckBox*> capture_boxes_;
    std::vector<QCheckBox*> check_boxes_;
};

} // namespace chesserazade::analyzer
