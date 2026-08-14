// Copyright (c) 2011, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file audio_player_alsa.cpp
/// @brief ALSA-based audio output
/// @ingroup audio_output
///

#ifdef WITH_ALSA
#include "include/aegisub/audio_player.h"

#include "audio_controller.h"
#include "compat.h"
#include "frame_main.h"
#include "options.h"

#include <libaegisub/audio/playback_renderer.h>
#include <libaegisub/audio/provider.h>
#include <libaegisub/log.h>

#include <atomic>
#include <algorithm>
#include <boost/scope_exit.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <alsa/asoundlib.h>
#include <memory>
#include <mutex>
#include <thread>

// X11 is the best
#undef None

namespace {
enum class Message {
	None,
	Start,
	Stop,
	Close
};

using clock = std::chrono::steady_clock;

class AlsaPlayer final : public AudioPlayer {
	std::mutex mutex;
	std::condition_variable cond;

	std::string device_name = OPT_GET("Player/Audio/ALSA/Device")->GetString();

	Message message = Message::None;

	std::atomic<bool> playing{false};
	std::atomic<double> volume{1.0};
	std::atomic<double> playback_speed{1.0};
	int64_t start_position = 0;
	std::atomic<int64_t> end_position{0};

	std::mutex position_mutex;
	int64_t last_position = 0;
	clock::time_point last_position_time;
	double last_position_speed = 1.0;

	std::vector<char> decode_buffer;

	std::thread thread;

	void PlaybackThread();

	void UpdatePlaybackPosition(snd_pcm_t *pcm, int64_t output_position,
	                            agi::AudioPlaybackRenderer const& renderer)
	{
		snd_pcm_sframes_t delay = 0;
		if (snd_pcm_delay(pcm, &delay) == 0)
		{
			auto played_output = std::max<int64_t>(0, output_position - std::max<snd_pcm_sframes_t>(0, delay));
			std::unique_lock<std::mutex> playback_lock(position_mutex);
			last_position = renderer.SourceFrameAtOutputFrame(played_output);
			last_position_time = clock::now();
			last_position_speed = renderer.GetSpeed();
		}
	}

public:
	AlsaPlayer(agi::AudioProvider *provider);
	~AlsaPlayer();

	void Play(int64_t start, int64_t count) override;
	void Stop() override;
	bool IsPlaying() override { return playing; }

	void SetVolume(double vol) override { volume = vol; }
	int64_t GetEndPosition() override { return end_position; }
	int64_t GetCurrentPosition() override;
	void SetEndPosition(int64_t pos) override;
	void SetPlaybackSpeed(double speed) override;
	bool SupportsPlaybackSpeed() const override { return true; }
};

void AlsaPlayer::PlaybackThread()
{
	std::unique_lock<std::mutex> lock(mutex);

	snd_pcm_t *pcm = nullptr;
	if (snd_pcm_open(&pcm, device_name.c_str(), SND_PCM_STREAM_PLAYBACK, 0) != 0)
		return;
	LOG_D("audio/player/alsa") << "opened pcm";
	BOOST_SCOPE_EXIT_ALL(&) { snd_pcm_close(pcm); };

do_setup:
	snd_pcm_format_t pcm_format;
	switch (provider->GetBytesPerSample())
	{
	case 1:
		LOG_D("audio/player/alsa") << "format U8";
		pcm_format = SND_PCM_FORMAT_U8;
		break;
	case 2:
		LOG_D("audio/player/alsa") << "format S16_LE";
		pcm_format = SND_PCM_FORMAT_S16_LE;
		break;
	default:
		return;
	}
	if (snd_pcm_set_params(pcm,
	                       pcm_format,
	                       SND_PCM_ACCESS_RW_INTERLEAVED,
	                       provider->GetChannels(),
	                       provider->GetSampleRate(),
	                       1, // allow resample
	                       100*1000 // 100 milliseconds latency
	                      ) != 0)
		return;
	LOG_D("audio/player/alsa") << "set pcm params";

	size_t framesize = provider->GetChannels() * provider->GetBytesPerSample();

	while (true) {
		while (message != Message::Start) {
			cond.wait(lock, [&] { return message != Message::None; });
			if (message == Message::Close)
				return;
			if (message == Message::Start && end_position > start_position)
				break;
			message = Message::None;
		}
		message = Message::None;

		LOG_D("audio/player/alsa") << "starting playback";
		agi::AudioPlaybackRenderer renderer(provider);
		renderer.Reset(start_position, end_position - start_position, playback_speed);
		int64_t output_written = 0;
		bool started = false;
		BOOST_SCOPE_EXIT_ALL(&) { playing = false; };

		while (true) {
			if (message == Message::Close) {
				snd_pcm_drop(pcm);
				return;
			}
			if (message == Message::Stop || message == Message::Start) {
				LOG_D("audio/player/alsa") << "playback loop, stop signal";
				snd_pcm_drop(pcm);
				break;
			}

			renderer.SetEndPosition(end_position);
			auto avail = snd_pcm_avail(pcm);
			if (avail == -EPIPE || avail == -ESTRPIPE) {
				if (snd_pcm_recover(pcm, avail, 1) < 0) {
					LOG_D("audio/player/alsa") << "failed to recover from underrun";
					return;
				}
				avail = snd_pcm_avail(pcm);
			}
			if (avail < 0) {
				LOG_D("audio/player/alsa") << "failed to query writable frames, avail=" << avail;
				return;
			}

			if (avail > 0 && !renderer.IsFinished()) {
				decode_buffer.resize(static_cast<size_t>(avail) * framesize);
				auto rendered = renderer.Render(
					reinterpret_cast<int16_t *>(decode_buffer.data()),
					static_cast<size_t>(avail), volume);

				size_t offset = 0;
				while (offset < rendered) {
					auto written = snd_pcm_writei(
						pcm, decode_buffer.data() + offset * framesize, rendered - offset);
					if (written == -ESTRPIPE || written == -EPIPE) {
						if (snd_pcm_recover(pcm, written, 0) < 0)
							return;
						continue;
					}
					if (written <= 0) {
						LOG_D("audio/player/alsa") << "error filling buffer, written=" << written;
						return;
					}
					offset += static_cast<size_t>(written);
					output_written += written;
				}

				if (!started && rendered > 0) {
					LOG_D("audio/player/alsa") << "initial buffer filled, hitting start";
					snd_pcm_start(pcm);
					started = true;
					playing = true;
				}
			}

			if (started)
				UpdatePlaybackPosition(pcm, output_written, renderer);

			if (renderer.IsFinished()) {
				LOG_D("audio/player/alsa") << "playback renderer finished, draining";
				if (started)
					snd_pcm_drain(pcm);
				{
					std::unique_lock<std::mutex> playback_lock(position_mutex);
					last_position = renderer.GetEndPosition();
					last_position_time = clock::now();
					last_position_speed = renderer.GetSpeed();
				}
				break;
			}

			cond.wait_for(lock, std::chrono::milliseconds{10});
		}

		playing = false;
		LOG_D("audio/player/alsa") << "out of playback loop";

		switch (snd_pcm_state(pcm))
		{
		case SND_PCM_STATE_OPEN:
			// no clue what could have happened here, but start over
			goto do_setup;

		case SND_PCM_STATE_SETUP:
			// we lost the preparedness?
			snd_pcm_prepare(pcm);
			break;

		case SND_PCM_STATE_DISCONNECTED:
			// lost device, close the handle and return error
			return;

		default:
			// everything else should either be fine or impossible (here)
			break;
		}
	}
}

AlsaPlayer::AlsaPlayer(agi::AudioProvider *provider) try
: AudioPlayer(provider)
, thread(&AlsaPlayer::PlaybackThread, this)
{
}
catch (std::system_error const&) {
	throw AudioPlayerOpenError("AlsaPlayer: Creating the playback thread failed");
}

AlsaPlayer::~AlsaPlayer()
{
	{
		std::unique_lock<std::mutex> lock(mutex);
		message = Message::Close;
		cond.notify_all();
	}

	thread.join();
}

void AlsaPlayer::Play(int64_t start, int64_t count)
{
	start = std::clamp<int64_t>(start, 0, provider->GetNumSamples());
	count = std::min(std::max<int64_t>(0, count), provider->GetNumSamples() - start);

	{
		std::unique_lock<std::mutex> playback_lock(position_mutex);
		last_position = start;
		last_position_time = std::chrono::steady_clock::now();
		last_position_speed = playback_speed.load();
	}

	std::unique_lock<std::mutex> lock(mutex);
	message = Message::Start;
	start_position = start;
	end_position = start + count;
	cond.notify_all();
}

void AlsaPlayer::Stop()
{
	std::unique_lock<std::mutex> lock(mutex);
	message = Message::Stop;
	cond.notify_all();
}

void AlsaPlayer::SetEndPosition(int64_t pos)
{
	std::unique_lock<std::mutex> lock(mutex);
	end_position = std::clamp<int64_t>(pos, start_position, provider->GetNumSamples());
}

int64_t AlsaPlayer::GetCurrentPosition()
{
	int64_t lastpos;
	clock::time_point lasttime;
	double speed;

	{
		std::unique_lock<std::mutex> playback_lock(position_mutex);
		lastpos = last_position;
		lasttime = last_position_time;
		speed = last_position_speed;
	}

	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - lasttime).count();
	auto extrapolated = lastpos + static_cast<int64_t>(ms * provider->GetSampleRate() * speed / 1000.0);
	return std::clamp<int64_t>(extrapolated, 0, end_position);
}

void AlsaPlayer::SetPlaybackSpeed(double speed)
{
	speed = std::max(0.01, speed);
	if (std::abs(playback_speed.exchange(speed) - speed) <= 0.0001 || !playing)
		return;

	auto current = GetCurrentPosition();
	std::unique_lock<std::mutex> lock(mutex);
	start_position = current;
	message = Message::Start;
	cond.notify_all();
}
}

std::unique_ptr<AudioPlayer> CreateAlsaPlayer(agi::AudioProvider *provider, wxWindow *)
{
	return std::make_unique<AlsaPlayer>(provider);
}

#endif // WITH_ALSA
