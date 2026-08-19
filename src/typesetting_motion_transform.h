// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include "typesetting_motion.h"

namespace typesetting::motion::detail {

/// Build the exact screen-space transform consumed by Apply for Mocha and
/// native Auto Motion samples. X and Y scale are deliberately independent.
Homography TransformMap(Sample const& current, Sample const& reference);

} // namespace typesetting::motion::detail
