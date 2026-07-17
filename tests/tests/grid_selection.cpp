// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#include <main.h>

#include "grid_selection_logic.h"

TEST(grid_selection, drag_selects_a_contiguous_range_in_both_directions) {
	EXPECT_EQ((std::set<int>{2, 3, 4, 5}), grid::DragSelectionRows(2, 5, {}, false));
	EXPECT_EQ((std::set<int>{2, 3, 4, 5}), grid::DragSelectionRows(5, 2, {}, false));
}

TEST(grid_selection, ctrl_drag_adds_the_range_to_the_initial_selection) {
	std::set<int> initial{0, 8};
	EXPECT_EQ((std::set<int>{0, 3, 4, 5, 8}),
		grid::DragSelectionRows(3, 5, initial, true));
}

TEST(grid_selection, non_additive_drag_replaces_the_initial_selection) {
	std::set<int> initial{0, 8};
	EXPECT_EQ((std::set<int>{3, 4, 5}),
		grid::DragSelectionRows(3, 5, initial, false));
}
