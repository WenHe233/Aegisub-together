// Copyright (c) 2026, Aegisub contributors

#include <main.h>

#include <libaegisub/audio/playback_renderer.h>
#include <libaegisub/audio/provider.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {
class SineProvider final : public agi::AudioProvider {
public:
	SineProvider(int64_t frames, int rate = 48000) {
		channels = 1;
		num_samples = frames;
		decoded_samples = frames;
		sample_rate = rate;
		bytes_per_sample = 2;
		float_samples = false;
	}

private:
	void FillBuffer(void *buffer, int64_t start, int64_t count) const override {
		auto output = static_cast<int16_t *>(buffer);
		constexpr double pi = 3.14159265358979323846;
		for (int64_t frame = 0; frame < count; ++frame)
			output[frame] = static_cast<int16_t>(std::sin((start + frame) * 2.0 * pi * 440.0 / sample_rate) * 12000.0);
	}
};

std::vector<int16_t> RenderAll(agi::AudioPlaybackRenderer& renderer) {
	std::vector<int16_t> result;
	std::vector<int16_t> block(257);
	while (!renderer.IsFinished()) {
		auto rendered = renderer.Render(block.data(), block.size(), 1.0);
		if (rendered == 0)
			break;
		result.insert(result.end(), block.begin(), block.begin() + rendered);
	}
	return result;
}
}

TEST(audio_playback_renderer, passthrough_range_and_position) {
	SineProvider provider(48000);
	agi::AudioPlaybackRenderer renderer(&provider);
	renderer.Reset(1000, 1234, 1.0);

	auto output = RenderAll(renderer);
	EXPECT_EQ(1234, output.size());
	EXPECT_TRUE(renderer.IsFinished());
	EXPECT_EQ(1000, renderer.SourceFrameAtOutputFrame(0));
	EXPECT_EQ(1617, renderer.SourceFrameAtOutputFrame(617));
	EXPECT_EQ(2234, renderer.SourceFrameAtOutputFrame(1234));
}

TEST(audio_playback_renderer, output_duration_tracks_speed) {
	SineProvider provider(96000);
	agi::AudioPlaybackRenderer renderer(&provider);

	renderer.Reset(0, 96000, 2.0);
	EXPECT_EQ(48000, RenderAll(renderer).size());

	renderer.Reset(0, 24000, 0.5);
	EXPECT_EQ(48000, RenderAll(renderer).size());
}

TEST(audio_playback_renderer, stretching_preserves_pitch) {
	SineProvider provider(96000);
	agi::AudioPlaybackRenderer renderer(&provider);
	renderer.Reset(0, 96000, 2.0);
	auto output = RenderAll(renderer);

	// Ignore the compensated startup and final blocks, then estimate frequency
	// from positive-going zero crossings in the steady-state section.
	size_t first = 4800;
	size_t last = output.size() - 4800;
	int crossings = 0;
	for (size_t i = first + 1; i < last; ++i) {
		if (output[i - 1] <= 0 && output[i] > 0)
			++crossings;
	}
	double seconds = static_cast<double>(last - first) / 48000.0;
	double frequency = crossings / seconds;
	EXPECT_GT(frequency, 420.0);
	EXPECT_LT(frequency, 460.0);
}
