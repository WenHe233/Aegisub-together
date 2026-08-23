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
class AssStyle;
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

/// Whether this row was written by the native codec, either kind.
bool IsNative(AssDialogue const *line);

/// Give a run of a mask's rows the names this codec uses: the two ends one way, everything between
/// them the other.
///
/// For a mask out of an older file, which names none of its rows - or names them all the same, as
/// the first version of this codec did. Left alone where any row carries an effect that is not
/// one of these, since that belongs to something else.
///
/// Returns whether anything was actually changed, so a caller need not do the rest of its work
/// when there was nothing to do.
bool NameRows(std::vector<AssDialogue *> const& rows);

/// Whether this row is one of a mask's middle rows rather than one of its two ends.
///
/// Old files name none of their rows this way, so a caller has to be ready for a run of mask rows
/// that says nothing about where one mask ends and the next begins - and treat the whole run as
/// the one mask it has always been.
bool IsElement(AssDialogue const *line);

/// Reconstruct a raster from either codec. The returned origin is in script pixels.
std::optional<Raster> Decode(std::vector<AssDialogue *> const& lines);

/// Encode RGBA pixels as Image2ASS-compatible, one-pixel-high rows with integer
/// geometry. Adjacent pixels already grouped by Prepare are emitted as one run.
std::vector<AssDialogue> Encode(Raster const& raster, AssDialogue const& prototype,
	int start_ms, int end_ms, AssStyle const *style,
	ProgressCallback progress = {});

/// Stable comparison key used to merge unchanged consecutive motion frames.
std::string Signature(std::vector<AssDialogue> const& lines);

} // namespace imagemask
