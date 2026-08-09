// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file video_color_pick.h
/// @brief Taking a colour off the video under the pointer

#pragma once

namespace agi { struct Context; }

namespace video_color_pick {

/// Which colour of the selected lines a pick writes.
enum class Target {
	Primary,   ///< \c, also written \1c
	Outline,   ///< \3c
	Shadow     ///< \4c
};

/// Whether there is a video and a frame to pick a colour out of, and a line to
/// write it to.
bool CanPick(const agi::Context *c);

/// One press of a colour-pick hotkey.
///
/// A single press takes the colour under the pointer and writes it to the selected
/// lines. Pressing the same one again straight away opens a magnified view of the
/// video around that point instead, for when the pixel is small or its edge matters,
/// and for choosing whether the colour goes in as a \t() transition.
void Invoke(agi::Context *c, Target target);

} // namespace video_color_pick
