// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "imagemask_codec.h"

#include <algorithm>
#include <cmath>

#include <wx/image.h>

namespace imagemask {
namespace {

double ColourDistance(double left_red, double left_green, double left_blue,
	double right_red, double right_green, double right_blue) {
	double red = left_red - right_red;
	double green = left_green - right_green;
	double blue = left_blue - right_blue;
	return std::sqrt(red * red + green * green + blue * blue);
}

void CompressRows(Raster& raster, int tolerance, ProgressCallback const& progress,
	size_t progress_offset, size_t progress_total) {
	for (int y = 0; y < raster.height; ++y) {
		int run_start = 0;
		int run_width = 1;
		size_t first = static_cast<size_t>(y) * raster.width * 4;
		double average_red = raster.rgba[first];
		double average_green = raster.rgba[first + 1];
		double average_blue = raster.rgba[first + 2];
		double deviation_red = 0;
		double deviation_green = 0;
		double deviation_blue = 0;
		double last_red = average_red;
		double last_green = average_green;
		double last_blue = average_blue;
		unsigned char run_alpha = raster.rgba[first + 3];
		auto flush = [&](int end) {
			unsigned char red = static_cast<unsigned char>(
				std::clamp(std::lround(average_red), 0l, 255l));
			unsigned char green = static_cast<unsigned char>(
				std::clamp(std::lround(average_green), 0l, 255l));
			unsigned char blue = static_cast<unsigned char>(
				std::clamp(std::lround(average_blue), 0l, 255l));
			for (int x = run_start; x < end; ++x) {
				size_t offset = (static_cast<size_t>(y) * raster.width + x) * 4;
				raster.rgba[offset] = red;
				raster.rgba[offset + 1] = green;
				raster.rgba[offset + 2] = blue;
			}
		};
		for (int x = 1; x < raster.width; ++x) {
			size_t current = (static_cast<size_t>(y) * raster.width + x) * 4;
			double red = raster.rgba[current];
			double green = raster.rgba[current + 1];
			double blue = raster.rgba[current + 2];
			double next_red = average_red + (red - average_red) / (run_width + 1);
			double next_green = average_green + (green - average_green) / (run_width + 1);
			double next_blue = average_blue + (blue - average_blue) / (run_width + 1);
			double next_deviation_red = deviation_red +
				(red - average_red) * (red - next_red);
			double next_deviation_green = deviation_green +
				(green - average_green) * (green - next_green);
			double next_deviation_blue = deviation_blue +
				(blue - average_blue) * (blue - next_blue);
			bool same_alpha = run_alpha == raster.rgba[current + 3];
			bool merge = same_alpha && (!run_alpha ||
				(ColourDistance(last_red, last_green, last_blue, red, green, blue) < tolerance &&
				 ColourDistance(next_deviation_red, next_deviation_green,
					next_deviation_blue, 0, 0, 0) < tolerance));
			if (merge) {
				average_red = next_red;
				average_green = next_green;
				average_blue = next_blue;
				deviation_red = next_deviation_red;
				deviation_green = next_deviation_green;
				deviation_blue = next_deviation_blue;
				++run_width;
			}
			else {
				flush(x);
				run_start = x;
				run_width = 1;
				average_red = red;
				average_green = green;
				average_blue = blue;
				deviation_red = deviation_green = deviation_blue = 0;
				run_alpha = raster.rgba[current + 3];
			}
			last_red = red;
			last_green = green;
			last_blue = blue;
		}
		flush(raster.width);
		if (progress) progress(progress_offset + y + 1, progress_total);
	}
}

} // namespace

std::optional<Raster> Prepare(wxImage const& source, wxImage const *alpha_source,
	ImportOptions const& raw_options, int x, int y, std::string& error,
	ProgressCallback progress) {
	error.clear();
	if (!source.IsOk()) {
		error = "The image could not be loaded.";
		return std::nullopt;
	}
	if (alpha_source && (!alpha_source->IsOk() ||
		alpha_source->GetWidth() != source.GetWidth() ||
		alpha_source->GetHeight() != source.GetHeight())) {
		error = "The alpha image must have the same dimensions as the source image.";
		return std::nullopt;
	}

	ImportOptions options = raw_options;
	options.compression = std::clamp(options.compression, 1, 3000);
	options.resize = std::clamp(options.resize, 1.0, 100.0);
	options.pixel_size = std::clamp(options.pixel_size, 1, 250);
	int width = std::max(1, static_cast<int>(std::lround(
		source.GetWidth() * options.resize / 100.0)));
	int height = std::max(1, static_cast<int>(std::lround(
		source.GetHeight() * options.resize / 100.0)));
	wxImage image = width == source.GetWidth() && height == source.GetHeight() ?
		source.Copy() : source.Scale(width, height, wxIMAGE_QUALITY_HIGH);
	wxImage alpha;
	if (alpha_source)
		alpha = width == alpha_source->GetWidth() && height == alpha_source->GetHeight() ?
			alpha_source->Copy() : alpha_source->Scale(width, height, wxIMAGE_QUALITY_HIGH);
	if (!image.IsOk() || (alpha_source && !alpha.IsOk())) {
		error = "The image could not be resized.";
		return std::nullopt;
	}

	Raster compact;
	compact.x = x;
	compact.y = y;
	compact.width = width;
	compact.height = height;
	compact.rgba.resize(static_cast<size_t>(width) * height * 4);
	size_t progress_phases = options.pixel_size == 1 ? 2 : 3;
	size_t progress_total = static_cast<size_t>(height) * progress_phases;
	auto rgb = image.GetData();
	auto native_alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
	auto alpha_rgb = alpha_source ? alpha.GetData() : nullptr;
	auto alpha_channel = alpha_source && alpha.HasAlpha() ? alpha.GetAlpha() : nullptr;
	for (int row = 0; row < height; ++row) {
		for (int column = 0; column < width; ++column) {
			size_t pixel = static_cast<size_t>(row) * width + column;
			compact.rgba[pixel * 4] = rgb[pixel * 3];
			compact.rgba[pixel * 4 + 1] = rgb[pixel * 3 + 1];
			compact.rgba[pixel * 4 + 2] = rgb[pixel * 3 + 2];
			int opacity = native_alpha ? native_alpha[pixel] : 255;
			if (alpha_source) {
				int grey = (static_cast<int>(alpha_rgb[pixel * 3]) +
					alpha_rgb[pixel * 3 + 1] + alpha_rgb[pixel * 3 + 2]) / 3;
				if (alpha_channel)
					grey = 255 - ((255 - grey) * alpha_channel[pixel] + 127) / 255;
				opacity = (opacity * (255 - grey) + 127) / 255;
			}
			compact.rgba[pixel * 4 + 3] = static_cast<unsigned char>(opacity);
		}
		if (progress) progress(static_cast<size_t>(row) + 1, progress_total);
	}
	CompressRows(compact, options.compression, progress,
		static_cast<size_t>(height), progress_total);
	if (options.pixel_size == 1) return compact;

	Raster expanded;
	expanded.x = compact.x;
	expanded.y = compact.y;
	expanded.width = compact.width * options.pixel_size;
	expanded.height = compact.height * options.pixel_size;
	expanded.rgba.resize(static_cast<size_t>(expanded.width) * expanded.height * 4);
	for (int source_y = 0; source_y < compact.height; ++source_y) {
		for (int source_x = 0; source_x < compact.width; ++source_x) {
			size_t from = (static_cast<size_t>(source_y) * compact.width + source_x) * 4;
			for (int offset_y = 0; offset_y < options.pixel_size; ++offset_y) {
				for (int offset_x = 0; offset_x < options.pixel_size; ++offset_x) {
					int target_x = source_x * options.pixel_size + offset_x;
					int target_y = source_y * options.pixel_size + offset_y;
					size_t to = (static_cast<size_t>(target_y) * expanded.width + target_x) * 4;
					std::copy_n(compact.rgba.begin() + from, 4, expanded.rgba.begin() + to);
				}
			}
		}
		if (progress) progress(static_cast<size_t>(height) * 2 + source_y + 1,
			progress_total);
	}
	return expanded;
}

} // namespace imagemask
