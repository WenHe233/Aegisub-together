// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

class AssDialogue;

namespace imagemask {

/// A pixel-exact image in script coordinates. Pixels are straight RGBA bytes and
/// x/y is the top-left corner on the ASS script canvas.
struct Raster {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	std::vector<unsigned char> rgba;

	bool IsOk() const {
		return width > 0 && height > 0 &&
			rgba.size() == static_cast<size_t>(width) * height * 4;
	}
};

/// Recognise both the native codec and old Image2ASS-compatible row drawings.
bool IsLine(AssDialogue const *line);

/// Reconstruct a raster from either codec. The returned origin is in script pixels.
std::optional<Raster> Decode(std::vector<AssDialogue *> const& lines);

/// Encode exact RGBA pixels with integer geometry. Only identical pixels are merged;
/// the encoder compares a vertically merged rectangle representation with a row
/// representation and keeps the shorter lossless result.
std::vector<AssDialogue> Encode(Raster const& raster, AssDialogue const& prototype,
	int start_ms, int end_ms);

/// Stable comparison key used to merge unchanged consecutive motion frames.
std::string Signature(std::vector<AssDialogue> const& lines);

} // namespace imagemask
