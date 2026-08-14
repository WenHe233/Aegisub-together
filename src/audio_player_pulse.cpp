// Copyright (c) 2007, Niels Martin Hansen
// Copyright (c) 2026, arch1t3cht
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

/// @file audio_player_pulse.cpp
/// @brief PulseAudio-based audio output
/// @ingroup audio_output
///

#ifdef WITH_LIBPULSE
#include "include/aegisub/audio_player.h"

#include "audio_controller.h"
#include "utils.h"

#include <libaegisub/audio/playback_renderer.h>
#include <libaegisub/audio/provider.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <pulse/pulseaudio.h>

namespace {

struct PAThreadedMainloopDeleter {
	void operator()(pa_threaded_mainloop *m) { pa_threaded_mainloop_free(m);}
};

struct PAOperationDeleter {
	void operator()(pa_operation *o) { pa_operation_unref(o);}
};

using PAThreadedMainloop = std::unique_ptr<pa_threaded_mainloop, PAThreadedMainloopDeleter>;
using PAOperation = std::unique_ptr<pa_operation, PAOperationDeleter>;

class PAThreadedMainloopLock {
	pa_threaded_mainloop *mainloop = nullptr;
public:
	[[nodiscard]] PAThreadedMainloopLock(pa_threaded_mainloop *mainloop) : mainloop(mainloop) {
		if (mainloop)
			pa_threaded_mainloop_lock(mainloop);
	}

	PAThreadedMainloopLock(const PAThreadedMainloopLock&) = delete;
	PAThreadedMainloopLock& operator=(const PAThreadedMainloopLock&) = delete;
	PAThreadedMainloopLock(PAThreadedMainloopLock&&) = delete;
	PAThreadedMainloopLock& operator=(PAThreadedMainloopLock&&) = delete;

	~PAThreadedMainloopLock() {
		if (mainloop)
			pa_threaded_mainloop_unlock(mainloop);
	}
};

// We cannot use std::unique_ptr for these since the mainloop needs to be locked
// during the destructor.
class PAContext {
	pa_context *context = nullptr;
	pa_threaded_mainloop *mainloop = nullptr;
	bool connected = false;

public:
	PAContext() = default;

	PAContext(const PAContext&) = delete;
	PAContext& operator=(const PAContext&) = delete;
	PAContext(PAContext&&) = delete;
	PAContext& operator=(PAContext&&) = delete;

	void reset(pa_context *context, pa_threaded_mainloop *mainloop) {
		this->context = context;
		this->mainloop = mainloop;
	}

	template<typename ...Args>
	[[nodiscard]] int connect(Args&&... args) {
		assert(!connected);
		PAThreadedMainloopLock lock{mainloop};
		int error = pa_context_connect(context, std::forward<Args>(args)...);
		if (error >= 0)
			connected = true;

		return error;
	}

	[[nodiscard]] int get_errno() {
		PAThreadedMainloopLock lock(mainloop);
		return pa_context_errno(context);
	}

	pa_context *get() { return context; }

	~PAContext() {
		if (context) {
			PAThreadedMainloopLock lock{mainloop};
			if (connected)
				pa_context_disconnect(context);

			pa_context_unref(context);
		}
	}
};

class PAStream {
	pa_stream *stream = nullptr;
	pa_threaded_mainloop *mainloop = nullptr;
	bool connected = false;

public:
	PAStream() = default;

	PAStream(const PAStream&) = delete;
	PAStream& operator=(const PAStream&) = delete;
	PAStream(PAStream&&) = delete;
	PAStream& operator=(PAStream&&) = delete;

	void reset(pa_stream *stream, pa_threaded_mainloop *mainloop) {
		this->stream = stream;
		this->mainloop = mainloop;
	}

	template<typename ...Args>
	[[nodiscard]] int connect(Args&& ...args) {
		assert(!connected);
		PAThreadedMainloopLock lock{mainloop};
		int error = pa_stream_connect_playback(stream, std::forward<Args>(args)...);
		if (!error)
			connected = true;

		return error;
	}

	pa_stream *get() { return stream; }

	~PAStream() {
		if (stream) {
			PAThreadedMainloopLock lock{mainloop};
			if (connected)
				pa_stream_disconnect(stream);

			pa_stream_unref(stream);
		}
	}
};

class PulseAudioPlayer final : public AudioPlayer {
	float volume = 1.f;
	bool is_playing = false;
	bool draining = false;
	double playback_speed = 1.0;

	int64_t start_frame = 0;
	int64_t end_frame = 0;

	size_t bpf = 0; // bytes per frame
	agi::AudioPlaybackRenderer renderer;

	int stream_success_val;
	pa_operation *drain_operation = nullptr;

	PAThreadedMainloop mainloop; // pulseaudio mainloop handle
	PAContext context; // connection context

	PAStream stream;

	pa_usec_t play_start_time; // timestamp when playback was started

	/// Called by PA to notify about other context-related stuff
	static void PAContextNotifyCB(pa_context *c, pa_threaded_mainloop *mainloop);
	/// Called by PA when a stream operation completes
	static void PAStreamSuccessCB(pa_stream *p, int success, PulseAudioPlayer *thread);
	/// Called by PA when all queued playback data has been heard
	static void PAStreamDrainCB(pa_stream *p, int success, PulseAudioPlayer *thread);
	/// Called by PA to request more data written to stream
	static void PAStreamWriteCB(pa_stream *p, size_t length, PulseAudioPlayer *thread);
	/// Called by PA to notify about other stream-related stuff
	static void PAStreamNotifyCB(pa_stream *p, pa_threaded_mainloop *mainloop);

	void CancelDrain();
	void BeginDrain(pa_stream *p);
	int64_t GetCurrentPositionUnlocked();

public:
	PulseAudioPlayer(agi::AudioProvider *provider);
	~PulseAudioPlayer();

	void Play(int64_t start,int64_t count);
	void Stop();
	bool IsPlaying();

	int64_t GetEndPosition();
	int64_t GetCurrentPosition();
	void SetEndPosition(int64_t pos);

	void SetVolume(double vol);
	void SetPlaybackSpeed(double speed) override;
	bool SupportsPlaybackSpeed() const override { return true; }
};

PulseAudioPlayer::PulseAudioPlayer(agi::AudioProvider *provider)
: AudioPlayer(provider)
, renderer(provider)
{
	// Initialise a mainloop
	mainloop.reset(pa_threaded_mainloop_new());
	if (!mainloop)
		throw AudioPlayerOpenError("Failed to initialise PulseAudio threaded mainloop object");

	if (pa_threaded_mainloop_start(mainloop.get()))
		throw AudioPlayerOpenError("Failed to start PulseAudio threaded mainloop");

	PAThreadedMainloopLock lock{mainloop.get()};

	// Create context
	context.reset(pa_context_new(pa_threaded_mainloop_get_api(mainloop.get()), "Aegisub"), mainloop.get());
	if (!context.get())
		throw AudioPlayerOpenError("Failed to create PulseAudio context");

	pa_context_set_state_callback(context.get(), (pa_context_notify_cb_t)PAContextNotifyCB, mainloop.get());

	// Connect the context
	if (context.connect(nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
		throw AudioPlayerOpenError("Failed to connect PulseAudio context");

	// Wait for connection
	while (true) {
		pa_threaded_mainloop_wait(mainloop.get());
		pa_context_state_t cstate = pa_context_get_state(context.get());

		if (cstate == PA_CONTEXT_READY) {
			break;
		} else if (cstate == PA_CONTEXT_FAILED) {
			// eww
			int paerror = context.get_errno();
			throw AudioPlayerOpenError(std::string("PulseAudio reported error: ") + pa_strerror(paerror));
		}
		// otherwise loop once more
	}

	// Set up stream
	bpf = provider->GetChannels() * provider->GetBytesPerSample();
	pa_sample_spec ss;
	ss.format = PA_SAMPLE_S16LE; // FIXME
	ss.rate = provider->GetSampleRate();
	ss.channels = provider->GetChannels();
	pa_channel_map map;
	if (!pa_channel_map_init_auto(&map, ss.channels, PA_CHANNEL_MAP_DEFAULT))
		throw AudioPlayerOpenError("Failed to initialize PulseAudio channel map");

	stream.reset(pa_stream_new(context.get(), "Sound", &ss, &map), mainloop.get());
	if (!stream.get()) {
		// argh!
		throw AudioPlayerOpenError("PulseAudio could not create stream");
	}
	pa_stream_set_state_callback(stream.get(), (pa_stream_notify_cb_t)PAStreamNotifyCB, mainloop.get());
	pa_stream_set_write_callback(stream.get(), (pa_stream_request_cb_t)PAStreamWriteCB, this);

	// Connect stream
	if (int paerror = stream.connect(nullptr, nullptr, (pa_stream_flags_t)(PA_STREAM_INTERPOLATE_TIMING|PA_STREAM_NOT_MONOTONOUS|PA_STREAM_AUTO_TIMING_UPDATE), nullptr, nullptr)) {
		LOG_E("audio/player/pulse") << "Stream connection failed: " << pa_strerror(paerror) << "(" << paerror << ")";
		throw AudioPlayerOpenError(std::string("PulseAudio reported error: ") + pa_strerror(paerror));
	}
	while (true) {
		pa_threaded_mainloop_wait(mainloop.get());
		pa_stream_state_t sstate = pa_stream_get_state(stream.get());
		if (sstate == PA_STREAM_READY) {
			break;
		} else if (sstate == PA_STREAM_FAILED) {
			int paerror = context.get_errno();
			LOG_E("audio/player/pulse") << "Stream connection failed: " << pa_strerror(paerror) << "(" << paerror << ")";
			throw AudioPlayerOpenError("PulseAudio player: Something went wrong connecting the stream");
		}
	}
}

PulseAudioPlayer::~PulseAudioPlayer()
{
	Stop();
}

void PulseAudioPlayer::CancelDrain()
{
	if (drain_operation) {
		pa_operation_cancel(drain_operation);
		pa_operation_unref(drain_operation);
		drain_operation = nullptr;
	}
	draining = false;
}

void PulseAudioPlayer::BeginDrain(pa_stream *p)
{
	if (draining)
		return;

	draining = true;
	drain_operation = pa_stream_drain(p, (pa_stream_success_cb_t)PAStreamDrainCB, this);
	if (!drain_operation) {
		draining = false;
		is_playing = false;
	}
}

void PulseAudioPlayer::Play(int64_t start,int64_t count)
{
	PAThreadedMainloopLock lock{mainloop.get()};
	if (is_playing || draining) {
		// If we're already playing, do a quick "reset"
		is_playing = false;
		CancelDrain();

		PAOperation op{pa_stream_flush(stream.get(), (pa_stream_success_cb_t)PAStreamSuccessCB, this)};

		while (pa_operation_get_state(op.get()) == PA_OPERATION_RUNNING)
			pa_threaded_mainloop_wait(mainloop.get());

		if (!stream_success_val) {
			int paerror = context.get_errno();
			LOG_E("audio/player/pulse") << "Error flushing stream: " << pa_strerror(paerror) << "(" << paerror << ")";
		}
	}

	renderer.Reset(start, count, playback_speed);
	start_frame = renderer.GetStartPosition();
	end_frame = renderer.GetEndPosition();

	is_playing = true;
	draining = false;

	play_start_time = 0;
	if (int paerror = pa_stream_get_time(stream.get(), (pa_usec_t*) &play_start_time))
		LOG_E("audio/player/pulse") << "Error getting stream time: " << pa_strerror(paerror) << "(" << paerror << ")";

	auto writable = pa_stream_writable_size(stream.get());
	if (writable != static_cast<size_t>(-1))
		PulseAudioPlayer::PAStreamWriteCB(stream.get(), writable, this);
	else
		LOG_E("audio/player/pulse") << "Error querying writable stream size";

	PAOperation op{pa_stream_trigger(stream.get(), (pa_stream_success_cb_t)PAStreamSuccessCB, this)};

	while (pa_operation_get_state(op.get()) == PA_OPERATION_RUNNING)
		pa_threaded_mainloop_wait(mainloop.get());

	if (!stream_success_val) {
		int paerror = context.get_errno();
		LOG_E("audio/player/pulse") << "Error triggering stream: " << pa_strerror(paerror) << "(" << paerror << ")";
	}
}

void PulseAudioPlayer::Stop()
{
	PAThreadedMainloopLock lock{mainloop.get()};
	if (!is_playing && !draining) return;

	is_playing = false;
	CancelDrain();

	start_frame = 0;
	end_frame = 0;

	// Flush the stream of data
	PAOperation op{pa_stream_flush(stream.get(), (pa_stream_success_cb_t)PAStreamSuccessCB, this)};

	while (pa_operation_get_state(op.get()) == PA_OPERATION_RUNNING)
		pa_threaded_mainloop_wait(mainloop.get());

	if (!stream_success_val) {
		int paerror = context.get_errno();
		LOG_E("audio/player/pulse") << "Error flushing stream: " << pa_strerror(paerror) << "(" << paerror << ")";
	}
}

void PulseAudioPlayer::SetEndPosition(int64_t pos)
{
	PAThreadedMainloopLock lock{mainloop.get()};
	renderer.SetEndPosition(pos);
	end_frame = renderer.GetEndPosition();
}

int64_t PulseAudioPlayer::GetCurrentPositionUnlocked()
{
	if (!is_playing) return 0;

	// FIXME: this should be based on not duration played but actual sample being heard
	// (during video playback, cur_frame might get changed to resync)

	// Calculation duration we have played, in microseconds
	pa_usec_t play_cur_time = play_start_time;
	if (int paerror = pa_stream_get_time(stream.get(), &play_cur_time)) {
		LOG_E("audio/player/pulse") << "Error getting stream time: " << pa_strerror(paerror) << "(" << paerror << ")";
		return start_frame;
	}
	pa_usec_t playtime = play_cur_time >= play_start_time ? play_cur_time - play_start_time : 0;

	auto output_frames = static_cast<int64_t>(playtime * provider->GetSampleRate() / (1000 * 1000));
	return renderer.SourceFrameAtOutputFrame(output_frames);
}

int64_t PulseAudioPlayer::GetCurrentPosition()
{
	PAThreadedMainloopLock lock{mainloop.get()};
	return GetCurrentPositionUnlocked();
}

int64_t PulseAudioPlayer::GetEndPosition()
{
	PAThreadedMainloopLock lock{mainloop.get()};
	return end_frame;
}

bool PulseAudioPlayer::IsPlaying()
{
	PAThreadedMainloopLock lock{mainloop.get()};
	return is_playing;
}

void PulseAudioPlayer::SetVolume(double vol)
{
	PAThreadedMainloopLock lock{mainloop.get()};
	volume = vol;
}

void PulseAudioPlayer::SetPlaybackSpeed(double speed)
{
	speed = std::max(0.01, speed);
	int64_t restart_from = 0;
	int64_t restart_count = 0;

	{
		PAThreadedMainloopLock lock{mainloop.get()};
		if (std::abs(playback_speed - speed) <= 0.0001)
			return;

		if (is_playing && !draining) {
			restart_from = GetCurrentPositionUnlocked();
			restart_count = std::max<int64_t>(0, end_frame - restart_from);
		}
		playback_speed = speed;
	}

	if (restart_count > 0)
		Play(restart_from, restart_count);
}

/// @brief Called by PA to notify about other context-related stuff
void PulseAudioPlayer::PAContextNotifyCB(pa_context *, pa_threaded_mainloop *mainloop)
{
	pa_threaded_mainloop_signal(mainloop, 0);
}

/// @brief Called by PA when an operation completes
void PulseAudioPlayer::PAStreamSuccessCB(pa_stream *, int success, PulseAudioPlayer *thread)
{
	thread->stream_success_val = success;
	pa_threaded_mainloop_signal(thread->mainloop.get(), 0);
}

void PulseAudioPlayer::PAStreamDrainCB(pa_stream *, int, PulseAudioPlayer *thread)
{
	if (thread->drain_operation) {
		pa_operation_unref(thread->drain_operation);
		thread->drain_operation = nullptr;
	}
	thread->draining = false;
	thread->is_playing = false;
	pa_threaded_mainloop_signal(thread->mainloop.get(), 0);
}

/// @brief Called by PA to request more data (and other things?)
void PulseAudioPlayer::PAStreamWriteCB(pa_stream *p, size_t length, PulseAudioPlayer *thread)
{
	if (!thread->is_playing || thread->draining) return;

	size_t frames = length / thread->bpf;
	if (frames == 0)
		return;

	void *buf = malloc(frames * thread->bpf);
	if (!buf) {
		LOG_E("audio/player/pulse") << "Failed to allocate playback buffer";
		return;
	}
	auto rendered = thread->renderer.Render(static_cast<int16_t *>(buf), frames, thread->volume);
	if (rendered > 0) {
		if (pa_stream_write(p, buf, rendered * thread->bpf, free, 0, PA_SEEK_RELATIVE)) {
			free(buf);
			LOG_E("audio/player/pulse") << "Error writing to stream";
		}
	}
	else {
		free(buf);
	}

	if (thread->renderer.IsFinished())
		thread->BeginDrain(p);
}

/// @brief Called by PA to notify about other stuff
void PulseAudioPlayer::PAStreamNotifyCB(pa_stream *, pa_threaded_mainloop *mainloop)
{
	pa_threaded_mainloop_signal(mainloop, 0);
}
}

std::unique_ptr<AudioPlayer> CreatePulseAudioPlayer(agi::AudioProvider *provider, wxWindow *) {
	return std::make_unique<PulseAudioPlayer>(provider);
}
#endif // WITH_LIBPULSE
