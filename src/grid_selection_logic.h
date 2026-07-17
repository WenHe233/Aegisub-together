// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <algorithm>
#include <set>

namespace grid {

inline std::set<int> DragSelectionRows(int anchor, int current,
	std::set<int> const& base_selection, bool additive)
{
	std::set<int> result;
	if (additive)
		result = base_selection;
	if (anchor < 0 || current < 0)
		return result;
	if (anchor > current)
		std::swap(anchor, current);
	for (int row = anchor; row <= current; ++row)
		result.insert(row);
	return result;
}

}
