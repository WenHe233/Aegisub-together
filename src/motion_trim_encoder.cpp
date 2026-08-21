// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "motion_trim_encoder.h"

#include "compat.h"
#include "include/aegisub/context.h"
#include "video_controller.h"
#include "video_frame.h"

#include <algorithm>
#include <cmath>

#include <wx/filefn.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

namespace {

std::string MediaError(std::string message, int status) {
	char detail[AV_ERROR_MAX_STRING_SIZE]{};
	av_strerror(status, detail, sizeof(detail));
	return message + ": " + detail;
}

class H264Writer {
	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	AVStream *stream = nullptr;
	AVFrame *converted = nullptr;
	AVPacket *packet = nullptr;
	SwsContext *converter = nullptr;

	bool Drain(std::string& error) {
		while (true) {
			int status = avcodec_receive_packet(codec, packet);
			if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return true;
			if (status < 0) {
				error = MediaError("A H.264 kódoló nem adott vissza képkockát", status);
				return false;
			}
			av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
			packet->stream_index = stream->index;
			status = av_interleaved_write_frame(format, packet);
			av_packet_unref(packet);
			if (status < 0) {
				error = MediaError("Az MP4 adatfolyam nem írható", status);
				return false;
			}
		}
	}

public:
	~H264Writer() {
		if (packet) av_packet_free(&packet);
		if (converted) av_frame_free(&converted);
		if (converter) sws_freeContext(converter);
		if (codec) avcodec_free_context(&codec);
		if (format) {
			if (format->pb && !(format->oformat->flags & AVFMT_NOFILE))
				avio_closep(&format->pb);
			avformat_free_context(format);
		}
	}

	bool Open(wxString const& output, VideoFrame const& source, double fps,
		int quality, std::string& error) {
		std::string filename = from_wx(output);
		int status = avformat_alloc_output_context2(&format, nullptr, "mp4", filename.c_str());
		if (status < 0 || !format) {
			error = status < 0 ? MediaError("Az MP4 tároló nem hozható létre", status) :
				"Az MP4 tároló nem hozható létre.";
			return false;
		}
		auto encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!encoder) {
			error = "A rendszer nem biztosít alkalmazáson belüli H.264 kódolót.";
			return false;
		}
		codec = avcodec_alloc_context3(encoder);
		if (!codec) { error = "A H.264 kódoló nem foglalható le."; return false; }
		codec->codec_id = AV_CODEC_ID_H264;
		codec->codec_type = AVMEDIA_TYPE_VIDEO;
		codec->width = source.width & ~1;
		codec->height = source.height & ~1;
		if (codec->width < 2 || codec->height < 2) {
			error = "A videó mérete nem alkalmas H.264 kódolásra.";
			return false;
		}
		AVRational frame_rate = av_d2q(std::clamp(fps, 1.0, 240.0), 1001000);
		codec->framerate = frame_rate;
		codec->time_base = av_inv_q(frame_rate);
		codec->pix_fmt = AV_PIX_FMT_YUV420P;
		codec->gop_size = std::max(1, static_cast<int>(std::lround(fps * 2.0)));
		codec->max_b_frames = 2;
		double quality_weight = std::clamp((51.0 - quality) / 33.0, .25, 1.5);
		codec->bit_rate = static_cast<int64_t>(std::clamp(
			codec->width * static_cast<double>(codec->height) * fps * .10 * quality_weight,
			750000.0, 50000000.0));
		if (format->oformat->flags & AVFMT_GLOBALHEADER)
			codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		AVDictionary *encoder_options = nullptr;
		std::string encoder_name = encoder->name ? encoder->name : "";
		if (encoder_name.find("libx264") != std::string::npos) {
			av_dict_set(&encoder_options, "crf", std::to_string(quality).c_str(), 0);
			av_dict_set(&encoder_options, "preset", "medium", 0);
		}
		else if (encoder_name.find("_mf") != std::string::npos) {
			av_dict_set(&encoder_options, "rate_control", "quality", 0);
			av_dict_set(&encoder_options, "quality",
				std::to_string(std::clamp(100 - quality, 0, 100)).c_str(), 0);
		}
		status = avcodec_open2(codec, encoder, &encoder_options);
		av_dict_free(&encoder_options);
		if (status < 0) {
			error = MediaError("A beépített H.264 kódoló nem indítható", status);
			return false;
		}

		stream = avformat_new_stream(format, nullptr);
		if (!stream) { error = "Az MP4 videósáv nem hozható létre."; return false; }
		stream->time_base = codec->time_base;
		stream->avg_frame_rate = frame_rate;
		status = avcodec_parameters_from_context(stream->codecpar, codec);
		if (status < 0) {
			error = MediaError("A H.264 videósáv nem állítható be", status);
			return false;
		}
		if (!(format->oformat->flags & AVFMT_NOFILE)) {
			status = avio_open(&format->pb, filename.c_str(), AVIO_FLAG_WRITE);
			if (status < 0) {
				error = MediaError("Az MP4 fájl nem nyitható meg írásra", status);
				return false;
			}
		}
		AVDictionary *mux_options = nullptr;
		av_dict_set(&mux_options, "movflags", "+faststart", 0);
		status = avformat_write_header(format, &mux_options);
		av_dict_free(&mux_options);
		if (status < 0) {
			error = MediaError("Az MP4 fejléc nem írható", status);
			return false;
		}
		converted = av_frame_alloc();
		packet = av_packet_alloc();
		if (!converted || !packet) {
			error = "A videókódoló képkockapuffere nem foglalható le.";
			return false;
		}
		converted->format = codec->pix_fmt;
		converted->width = codec->width;
		converted->height = codec->height;
		status = av_frame_get_buffer(converted, 32);
		if (status < 0) {
			error = MediaError("A videókódoló képkockapuffere nem hozható létre", status);
			return false;
		}
		converter = sws_getContext(source.width, source.height, AV_PIX_FMT_BGRA,
			codec->width, codec->height, codec->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
		if (!converter) {
			error = "A videó színformátuma nem alakítható át H.264 kódoláshoz.";
			return false;
		}
		return true;
	}

	bool Write(VideoFrame const& source, int64_t pts, std::string& error) {
		if (source.width <= 0 || source.height <= 0 || source.data.empty()) {
			error = "Egy forrásképkocka érvénytelen.";
			return false;
		}
		int status = av_frame_make_writable(converted);
		if (status < 0) {
			error = MediaError("A videókódoló képkockapuffere nem írható", status);
			return false;
		}
		uint8_t const *source_data = source.data.data();
		int source_stride = source.pitch;
		if (source.flipped) {
			source_data += static_cast<size_t>(source.height - 1) * source.pitch;
			source_stride = -source_stride;
		}
		uint8_t const *planes[] = {source_data, nullptr, nullptr, nullptr};
		int strides[] = {source_stride, 0, 0, 0};
		status = sws_scale(converter, planes, strides, 0, source.height,
			converted->data, converted->linesize);
		if (status <= 0) {
			error = "Egy forrásképkocka nem alakítható át H.264 kódoláshoz.";
			return false;
		}
		converted->pts = pts;
		status = avcodec_send_frame(codec, converted);
		if (status < 0) {
			error = MediaError("A képkocka nem adható át a H.264 kódolónak", status);
			return false;
		}
		return Drain(error);
	}

	bool Finish(std::string& error) {
		int status = avcodec_send_frame(codec, nullptr);
		if (status < 0 && status != AVERROR_EOF) {
			error = MediaError("A H.264 kódoló nem zárható le", status);
			return false;
		}
		if (!Drain(error)) return false;
		status = av_write_trailer(format);
		if (status < 0) {
			error = MediaError("Az MP4 fájl nem zárható le", status);
			return false;
		}
		return true;
	}
};

} // namespace

bool EncodeMotionTrimH264(agi::Context *context, wxString const& output,
	int first_frame, int last_frame, int quality,
	std::function<bool(int, int)> progress, std::string& error) {
	error.clear();
	if (!context || !context->videoController || first_frame > last_frame) {
		error = "A H.264 trim képkockatartománya érvénytelen.";
		return false;
	}
	int frame_count = last_frame - first_frame + 1;
	int duration = context->videoController->TimeAtFrame(last_frame, agi::vfr::END) -
		context->videoController->TimeAtFrame(first_frame, agi::vfr::START);
	double fps = frame_count * 1000.0 / std::max(1, duration);
	bool complete = [&] {
		auto first = context->videoController->GetFrame(first_frame, true);
		if (!first) { error = "Az első forrásképkocka nem olvasható."; return false; }
		H264Writer writer;
		if (!writer.Open(output, *first, fps, quality, error)) return false;
		for (int frame = first_frame; frame <= last_frame; ++frame) {
			int done = frame - first_frame;
			if (progress && !progress(done, frame_count)) {
				error = "A H.264 trim exportálása megszakadt.";
				return false;
			}
			auto source = frame == first_frame ? first :
				context->videoController->GetFrame(frame, true);
			if (!source) { error = "Egy forrásképkocka nem olvasható."; return false; }
			if (!writer.Write(*source, done, error)) return false;
		}
		return writer.Finish(error);
	}();
	if (!complete && wxFileExists(output)) wxRemoveFile(output);
	return complete;
}
