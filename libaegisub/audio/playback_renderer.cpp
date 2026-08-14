// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include <libaegisub/audio/playback_renderer.h>

#include <libaegisub/audio/provider.h>
#include <libaegisub/exception.h>

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace agi {
namespace {
constexpr size_t process_block_size = 512;

int64_t ExpectedOutputFrames(int64_t source_frames, double speed) {
	if (source_frames <= 0)
		return 0;

	auto frames = std::ceil(static_cast<long double>(source_frames) / speed);
	if (frames >= static_cast<long double>(std::numeric_limits<int64_t>::max()))
		return std::numeric_limits<int64_t>::max();
	return static_cast<int64_t>(frames);
}
}

struct AudioPlaybackRenderer::Impl {
	AudioProvider *provider;
	int channels;

	int64_t start = 0;
	int64_t end = 0;
	int64_t input_position = 0;
	int64_t expected_output_frames = 0;
	int64_t rendered_output_frames = 0;
	double speed = 1.0;

	bool passthrough = true;
	bool input_finalized = false;
	bool stretcher_finished = false;
	size_t start_delay_remaining = 0;

	std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
	std::vector<int16_t> input_pcm;
	std::vector<std::vector<float>> channel_buffers;
	std::vector<float *> channel_ptrs;

	explicit Impl(AudioProvider *provider)
	: provider(provider)
	, channels(provider ? provider->GetChannels() : 0)
	{
		if (!provider)
			throw InternalError("AudioPlaybackRenderer requires an audio provider");
		if (provider->AreSamplesFloat() || provider->GetBytesPerSample() != 2)
			throw InternalError("AudioPlaybackRenderer requires signed 16-bit PCM");
		if (channels <= 0)
			throw InternalError("AudioPlaybackRenderer requires at least one channel");
	}

	void PrepareChannelBuffers(size_t frames) {
		channel_buffers.resize(channels);
		channel_ptrs.resize(channels);
		for (int channel = 0; channel < channels; ++channel) {
			channel_buffers[channel].resize(frames);
			channel_ptrs[channel] = channel_buffers[channel].data();
		}
	}

	void ProcessSilence(size_t frames) {
		while (frames > 0) {
			auto block = std::min(frames, process_block_size);
			PrepareChannelBuffers(block);
			for (auto& channel : channel_buffers)
				std::fill_n(channel.data(), block, 0.0f);
			stretcher->process(channel_ptrs.data(), block, false);
			frames -= block;
		}
	}

	void FeedInput(double volume) {
		if (input_finalized)
			return;

		auto required = stretcher->getSamplesRequired();
		if (required == 0)
			required = process_block_size;
		required = std::min(required, process_block_size);

		auto remaining = std::max<int64_t>(0, end - input_position);
		auto count = static_cast<size_t>(std::min<int64_t>(remaining, required));
		bool final = count == static_cast<size_t>(remaining);

		if (count == 0) {
			stretcher->process(nullptr, 0, true);
			input_finalized = true;
			return;
		}

		input_pcm.resize(count * channels);
		provider->GetAudioWithVolume(input_pcm.data(), input_position, count, volume);
		PrepareChannelBuffers(count);

		for (size_t frame = 0; frame < count; ++frame) {
			for (int channel = 0; channel < channels; ++channel)
				channel_buffers[channel][frame] = input_pcm[frame * channels + channel] / 32768.0f;
		}

		stretcher->process(channel_ptrs.data(), count, final);
		input_position += count;
		input_finalized = final;
	}

	size_t Retrieve(int16_t *output, size_t frames, bool discard) {
		PrepareChannelBuffers(frames);
		auto retrieved = static_cast<size_t>(stretcher->retrieve(channel_ptrs.data(), frames));
		if (discard || retrieved == 0)
			return retrieved;

		for (size_t frame = 0; frame < retrieved; ++frame) {
			for (int channel = 0; channel < channels; ++channel) {
				float sample = std::clamp(channel_buffers[channel][frame], -1.0f, 1.0f);
				output[frame * channels + channel] = static_cast<int16_t>(std::lrint(sample * 32767.0f));
			}
		}
		return retrieved;
	}

	void Reset(int64_t new_start, int64_t count, double new_speed) {
		start = std::clamp<int64_t>(new_start, 0, provider->GetNumSamples());
		auto const max_count = provider->GetNumSamples() - start;
		end = start + std::min(std::max<int64_t>(0, count), max_count);
		input_position = start;
		speed = std::max(0.01, new_speed);
		expected_output_frames = ExpectedOutputFrames(end - start, speed);
		rendered_output_frames = 0;
		passthrough = std::abs(speed - 1.0) <= 0.0001;
		input_finalized = false;
		stretcher_finished = false;
		start_delay_remaining = 0;
		stretcher.reset();

		if (passthrough || expected_output_frames == 0)
			return;

		auto options = RubberBand::RubberBandStretcher::OptionProcessRealTime |
			RubberBand::RubberBandStretcher::OptionWindowShort |
			RubberBand::RubberBandStretcher::OptionThreadingAuto |
			RubberBand::RubberBandStretcher::OptionFormantPreserved;
		stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
			provider->GetSampleRate(), channels, options);
		stretcher->setMaxProcessSize(process_block_size);
		stretcher->setTimeRatio(1.0 / speed);

		// Real-time mode requires explicit input padding and output-delay
		// compensation to align the first audible output frame with start.
		auto preferred_pad = stretcher->getPreferredStartPad();
		start_delay_remaining = stretcher->getStartDelay();
		ProcessSilence(preferred_pad);
	}

	size_t Render(int16_t *output, size_t frames, double volume) {
		if (!output || frames == 0 || rendered_output_frames >= expected_output_frames)
			return 0;

		auto remaining_output = expected_output_frames - rendered_output_frames;
		auto requested = static_cast<size_t>(std::min<int64_t>(remaining_output, frames));

		if (passthrough) {
			auto source_remaining = std::max<int64_t>(0, end - input_position);
			auto count = static_cast<size_t>(std::min<int64_t>(source_remaining, requested));
			if (count > 0)
				provider->GetAudioWithVolume(output, input_position, count, volume);
			input_position += count;
			rendered_output_frames += count;
			return count;
		}

		size_t obtained = 0;
		while (obtained < requested) {
			int available = stretcher->available();
			if (available < 0) {
				stretcher_finished = true;
				break;
			}
			if (available == 0) {
				if (input_finalized) {
					stretcher_finished = true;
					break;
				}
				FeedInput(volume);
				continue;
			}

			if (start_delay_remaining > 0) {
				auto count = std::min<size_t>(start_delay_remaining, available);
				auto discarded = Retrieve(nullptr, count, true);
				start_delay_remaining -= discarded;
				continue;
			}

			auto count = std::min<size_t>(requested - obtained, available);
			auto retrieved = Retrieve(output + obtained * channels, count, false);
			if (retrieved == 0)
				break;
			obtained += retrieved;
		}

		// Rubber Band may round the final real-time block down or up. Truncate
		// excess above, and pad a short final block so wall-clock duration stays
		// exactly tied to the selected source range and speed.
		if (obtained < requested && stretcher_finished) {
			std::fill(output + obtained * channels, output + requested * channels, 0);
			obtained = requested;
		}

		rendered_output_frames += obtained;
		return obtained;
	}

	void SetEndPosition(int64_t new_end) {
		end = std::clamp<int64_t>(new_end, start, provider->GetNumSamples());
		expected_output_frames = ExpectedOutputFrames(end - start, speed);
	}

	int64_t SourceFrameAtOutputFrame(int64_t output_frame) const {
		if (output_frame <= 0)
			return start;
		auto source_offset = static_cast<int64_t>(std::llround(output_frame * speed));
		return std::clamp<int64_t>(start + source_offset, start, end);
	}
};

AudioPlaybackRenderer::AudioPlaybackRenderer(AudioProvider *provider)
: impl(std::make_unique<Impl>(provider))
{
}

AudioPlaybackRenderer::~AudioPlaybackRenderer() = default;

void AudioPlaybackRenderer::Reset(int64_t start, int64_t count, double speed) {
	impl->Reset(start, count, speed);
}

size_t AudioPlaybackRenderer::Render(int16_t *output, size_t frames, double volume) {
	return impl->Render(output, frames, volume);
}

void AudioPlaybackRenderer::SetEndPosition(int64_t end) {
	impl->SetEndPosition(end);
}

bool AudioPlaybackRenderer::IsFinished() const {
	return impl->rendered_output_frames >= impl->expected_output_frames;
}

double AudioPlaybackRenderer::GetSpeed() const { return impl->speed; }
int64_t AudioPlaybackRenderer::GetStartPosition() const { return impl->start; }
int64_t AudioPlaybackRenderer::GetEndPosition() const { return impl->end; }
int64_t AudioPlaybackRenderer::GetExpectedOutputFrames() const { return impl->expected_output_frames; }
int64_t AudioPlaybackRenderer::GetRenderedOutputFrames() const { return impl->rendered_output_frames; }

int64_t AudioPlaybackRenderer::SourceFrameAtOutputFrame(int64_t output_frame) const {
	return impl->SourceFrameAtOutputFrame(output_frame);
}
}
