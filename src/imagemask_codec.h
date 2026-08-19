// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class AssDialogue;
class wxImage;

namespace imagemask {

using ProgressCallback = std::function<void(size_t complete, size_t total)>;

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

/// Options shared by image insertion and every feature which turns a bitmap into
/// ImageMask ASS drawings. Compression uses Image2ASS's running colour average and
/// variance limit; 1 is effectively pixel-exact and 40 is the script default.
/// Resize is applied before pixel_size expands each source pixel to an integer
/// square on the script canvas.
struct ImportOptions {
	int compression = 40;
	double resize = 100.0;
	int pixel_size = 1;
};

/// Convert a wxImage (and optional legacy Image2ASS grayscale alpha image) to the
/// common integer-grid raster consumed by Encode. The alpha image must have the
/// source image's original dimensions; black is opaque and white is transparent.
std::optional<Raster> Prepare(wxImage const& image, wxImage const *alpha_image,
	ImportOptions const& options, int x, int y, std::string& error,
	ProgressCallback progress = {});

/// Recognise both the native codec and old Image2ASS-compatible row drawings.
bool IsLine(AssDialogue const *line);

/// Reconstruct a raster from either codec. The returned origin is in script pixels.
std::optional<Raster> Decode(std::vector<AssDialogue *> const& lines);

/// Encode RGBA pixels as Image2ASS-compatible, one-pixel-high rows with integer
/// geometry. Adjacent pixels already grouped by Prepare are emitted as one run.
std::vector<AssDialogue> Encode(Raster const& raster, AssDialogue const& prototype,
	int start_ms, int end_ms, ProgressCallback progress = {});

/// Stable comparison key used to merge unchanged consecutive motion frames.
std::string Signature(std::vector<AssDialogue> const& lines);

} // namespace imagemask
