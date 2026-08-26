// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_image_insert.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "dialog_progress.h"
#include "export_fixstyle.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/subtitles_provider.h"
#include "imagemask_codec.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "subtitle_line_combiner.h"
#include "typesetting_glitch.h"
#include "video_controller.h"
#include "video_frame.h"

#include <boost/gil.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wx/base64.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/filesys.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/zipstrm.h>

#if wxUSE_WEBVIEW
#include <wx/webview.h>
#endif

namespace typesetting::image_insert {
namespace {

struct EditorImages {
	wxImage frame;
	struct SubtitleLayer {
		wxString name;
		wxImage image;
	};
	std::vector<SubtitleLayer> subtitles;
};

struct SubtitleLayerSource {
	wxString name;
	std::vector<AssDialogue *> lines;
	int layer = 0;
};

bool IsDrawing(AssDialogue const& line) {
	if (IsImageMaskLine(&line)) return true;
	for (auto const& block : line.ParseTags())
		if (block->GetType() == AssBlockType::DRAWING) return true;
	return false;
}

std::vector<SubtitleLayerSource> CollectSubtitleLayers(agi::Context *context) {
	std::set<AssDialogue *> selected(
		context->selectionController->GetSelectedSet().begin(),
		context->selectionController->GetSelectedSet().end());
	std::set<AssDialogue *> claimed;
	std::vector<SubtitleLayerSource> result;
	int shape_index = 0;

	for (auto& event : context->ass->Events) {
		auto *line = &event;
		if (!selected.count(line) || claimed.count(line)) continue;

		std::vector<AssDialogue *> lines{line};
		if (context->imageMask) {
			auto const& group = context->imageMask->GetGroupLines(line);
			if (!group.empty()) lines.assign(group.begin(), group.end());
		}
		claimed.insert(lines.begin(), lines.end());

		std::string label;
		if (context->imageMask &&
			(context->imageMask->IsGradientGroup(line) ||
				context->imageMask->IsTextBoxGroup(line)))
			label = context->imageMask->GetGroupLabel(line);
		if (label.empty()) label = line->GetStrippedText();

		wxString name = to_wx(label);
		name.Trim(true).Trim(false);
		bool drawing = std::any_of(lines.begin(), lines.end(),
			[](AssDialogue const *candidate) { return IsDrawing(*candidate); });
		if (name.empty()) {
			name = drawing ? fmt_tl("Shape %d", ++shape_index) :
				_("Subtitle");
		}
		result.push_back({std::move(name), std::move(lines), line->Layer});
	}

	std::stable_sort(result.begin(), result.end(),
		[](SubtitleLayerSource const& left, SubtitleLayerSource const& right) {
			return left.layer < right.layer;
		});
	return result;
}

wxImage RenderSubtitleLayer(agi::Context *context, SubtitlesProvider *provider,
	std::vector<AssDialogue *> const& layer_lines, int time, int width, int height) {
	if (!layer_lines.empty() &&
		std::all_of(layer_lines.begin(), layer_lines.end(),
			[](AssDialogue const *line) { return IsImageMaskLine(line); })) {
		auto raster = imagemask::Decode(layer_lines);
		if (raster && raster->IsOk()) {
			int script_width = 0;
			int script_height = 0;
			context->ass->GetResolution(script_width, script_height);
			if (script_width > 0 && script_height > 0) {
				wxImage image(script_width, script_height, true);
				image.InitAlpha();
				auto *rgb = image.GetData();
				auto *alpha = image.GetAlpha();
				std::fill_n(alpha, static_cast<size_t>(script_width) * script_height, 0);
				int left = std::max(0, raster->x);
				int top = std::max(0, raster->y);
				int right = std::min(script_width, raster->x + raster->width);
				int bottom = std::min(script_height, raster->y + raster->height);
				for (int y = top; y < bottom; ++y) {
					for (int x = left; x < right; ++x) {
						size_t source = (static_cast<size_t>(y - raster->y) *
							raster->width + x - raster->x) * 4;
						size_t target = static_cast<size_t>(y) * script_width + x;
						rgb[target * 3] = raster->rgba[source];
						rgb[target * 3 + 1] = raster->rgba[source + 1];
						rgb[target * 3 + 2] = raster->rgba[source + 2];
						alpha[target] = raster->rgba[source + 3];
					}
				}
				if (script_width != width || script_height != height)
					image = image.Scale(width, height, wxIMAGE_QUALITY_HIGH);
				return image;
			}
		}
	}

	AssFile selected(*context->ass);
	std::set<AssDialogue *> wanted(layer_lines.begin(), layer_lines.end());
	auto source = context->ass->Events.begin();
	auto copy = selected.Events.begin();
	for (; source != context->ass->Events.end() && copy != selected.Events.end();
		++source, ++copy) {
		if (!wanted.count(&*source)) {
			copy->Comment = true;
			continue;
		}
		if (!copy->Comment) {
			copy->Start = time;
			copy->End = time + 1000;
		}
	}

	AssFixStylesFilter::ProcessSubs(&selected);

	auto blank_frame = [=](bool white) {
		VideoFrame result;
		result.width = width;
		result.height = height;
		result.pitch = width * 4;
		result.flipped = false;
		result.data.resize(result.pitch * result.height, white ? 255 : 0);
		return result;
	};
	VideoFrame black = blank_frame(false);
	VideoFrame white = blank_frame(true);
	provider->LoadSubtitles(&selected);
	provider->DrawSubtitles(black, time / 1000.);
	provider->DrawSubtitles(white, time / 1000.);

	using namespace boost::gil;
	auto black_view = interleaved_view(width, height,
		reinterpret_cast<bgra8_pixel_t *>(black.data.data()), black.pitch);
	auto white_view = interleaved_view(width, height,
		reinterpret_cast<bgra8_pixel_t *>(white.data.data()), white.pitch);
	transform_pixels(black_view, white_view, black_view,
		[](bgra8_pixel_t const black_pixel, bgra8_pixel_t const white_pixel) {
			int alpha = 255 - (white_pixel[0] - black_pixel[0]);
			bgra8_pixel_t result{};
			if (alpha) {
				result[0] = black_pixel[0] / (alpha / 255.);
				result[1] = black_pixel[1] / (alpha / 255.);
				result[2] = black_pixel[2] / (alpha / 255.);
				result[3] = alpha;
			}
			return result;
		});
	return GetImageWithAlpha(black);
}

std::optional<EditorImages> CaptureEditorImages(agi::Context *context) {
	if (!context->project->VideoProvider()) {
		wxMessageBox(_("Open a video before starting ImageEditor."),
			_("Image Editor"), wxOK | wxICON_INFORMATION, context->parent);
		return std::nullopt;
	}
	int frame_number = context->videoController->GetFrameN();
	auto frame = context->videoController->GetFrame(frame_number, true);
	if (!frame) {
		wxMessageBox(_("The current video frame could not be loaded."),
			_("Image Editor"), wxOK | wxICON_WARNING, context->parent);
		return std::nullopt;
	}
	try {
		int time = context->videoController->TimeAtFrame(frame_number);
		auto sources = CollectSubtitleLayers(context);
		DialogProgress progress(context->parent, _("Image Editor"),
			_("Rendering the selected subtitles..."));
		auto provider = SubtitlesProviderFactory::GetProvider(&progress);
		std::vector<EditorImages::SubtitleLayer> layers;
		layers.reserve(sources.size());
		for (auto const& source : sources) {
			layers.push_back({source.name, RenderSubtitleLayer(context, provider.get(),
				source.lines, time, frame->width, frame->height)});
		}
		return EditorImages{GetImage(*frame), std::move(layers)};
	}
	catch (std::exception const& error) {
		wxMessageBox(to_wx(error.what()), _("Image Editor"),
			wxOK | wxICON_WARNING, context->parent);
	}
	catch (std::string const& error) {
		wxMessageBox(to_wx(error), _("Image Editor"),
			wxOK | wxICON_WARNING, context->parent);
	}
	catch (...) {
		wxMessageBox(_("The selected subtitles could not be rendered."),
			_("Image Editor"), wxOK | wxICON_WARNING, context->parent);
	}
	return std::nullopt;
}

#if wxUSE_WEBVIEW

#if wxCHECK_VERSION(3, 3, 0)
class MemoryResponseData final : public wxWebViewHandlerResponseData {
	std::vector<unsigned char> data;
	wxMemoryInputStream stream;

public:
	explicit MemoryResponseData(std::vector<unsigned char> value)
	: data(std::move(value)), stream(data.data(), data.size()) {
	}

	wxInputStream *GetStream() override { return &stream; }
};
#endif

wxString ContentType(wxString const& path) {
	if (path.EndsWith(".html")) return "text/html; charset=utf-8";
	if (path.EndsWith(".js")) return "application/javascript; charset=utf-8";
	if (path.EndsWith(".css")) return "text/css; charset=utf-8";
	if (path.EndsWith(".wasm")) return "application/wasm";
	if (path.EndsWith(".png")) return "image/png";
	if (path.EndsWith(".zip")) return "application/zip";
	return "application/octet-stream";
}

wxString ResourcePath(wxString uri) {
	size_t scheme = uri.find("://");
	if (scheme != wxString::npos) {
		uri = uri.Mid(scheme + 3);
		size_t slash = uri.find('/');
		uri = slash == wxString::npos ? wxString() : uri.Mid(slash + 1);
	}
	else {
		size_t colon = uri.find(':');
		if (colon != wxString::npos) uri = uri.Mid(colon + 1);
		while (uri.StartsWith("/")) uri = uri.Mid(1);
	}
	size_t query = uri.find_first_of("?#");
	if (query != wxString::npos) uri = uri.Left(query);
	uri.Replace("\\", "/");
	if (uri.empty() || uri.Contains("../") || uri.StartsWith("../"))
		return {};
	return uri;
}

class ImageEditorResourceHandler final : public wxWebViewHandler {
	std::map<wxString, std::vector<unsigned char>> files;

public:
	ImageEditorResourceHandler() : wxWebViewHandler("imageeditor") {
		wxMemoryInputStream memory(image_editor_bundle, sizeof(image_editor_bundle));
		wxZipInputStream archive(memory);
		while (auto raw_entry = archive.GetNextEntry()) {
			std::unique_ptr<wxZipEntry> entry(raw_entry);
			if (entry->IsDir()) continue;
			wxMemoryOutputStream output;
			archive.Read(output);
			if (!archive.IsOk() && !archive.Eof()) {
				files.clear();
				return;
			}
			std::vector<unsigned char> bytes(
				static_cast<size_t>(output.GetLength()));
			if (!bytes.empty()) output.CopyTo(bytes.data(), bytes.size());
			wxString name = entry->GetName();
			name.Replace("\\", "/");
			files.emplace(std::move(name), std::move(bytes));
		}
	}

	bool IsUsable() const { return !files.empty(); }

	wxFSFile *GetFile(wxString const& uri) override {
		wxString path = ResourcePath(uri);
		auto found = files.find(path);
		if (found == files.end()) return nullptr;

		return new wxFSFile(
			new wxMemoryInputStream(found->second.data(), found->second.size()),
			uri, ContentType(path), wxString()
#if wxUSE_DATETIME
			, wxDateTime::Now()
#endif
		);
	}

#if wxCHECK_VERSION(3, 3, 0)
	void StartRequest(const wxWebViewHandlerRequest& request,
		wxSharedPtr<wxWebViewHandlerResponse> response) override {
		wxString path = ResourcePath(request.GetURI());
		auto found = files.find(path);
		if (found == files.end()) {
			response->SetStatus(404);
			response->FinishWithError();
			return;
		}
		response->SetStatus(200);
		response->SetContentType(ContentType(path));
		response->SetHeader("Access-Control-Allow-Origin", "*");
		response->SetHeader("Cache-Control", "no-store");
		response->Finish(wxSharedPtr<wxWebViewHandlerResponseData>(
			new MemoryResponseData(found->second)));
	}
#endif
};

wxString BrowserBackend() {
#ifdef __WXMSW__
	return wxWebViewBackendEdge;
#else
	return wxWebViewBackendDefault;
#endif
}

wxString EditorLanguage() {
	wxString language = to_wx(OPT_GET("App/Language")->GetString());
	if (language.empty()) return "en";
	language.Replace("_", "-");
	for (auto character : language) {
		if (!(wxIsalnum(character) || character == '-')) return "en";
	}
	return language;
}

wxString EditorUrl() {
	return "imageeditor://app/code/custom/image-editor-wrapper.html?lang=" +
		EditorLanguage();
}

wxString BridgeScript() {
	auto script = wxString::FromUTF8(R"JS((function () {
	"use strict";
	if (window.top !== window) return;
	window.__imageEditorInput = "";
	window.__imageEditorSubtitleInputs = [];
	window.__imageEditorSubtitleNames = [];
	var saving = false;
	var inputSent = false;
	var scenePrepared = false;
	var subtitleIndex = 0;
	var subtitleOpen = false;
	var waitingForNextSubtitle = false;
	var editorDocumentCount = 0;
	var sceneWasVisible = true;
	var sceneLayerName = "__IMAGE_EDITOR_SCENE_LAYER__";
	var subtitleGroupName = "__IMAGE_EDITOR_SUBTITLE_GROUP__";

	function notify(message) {
		if (window.aegisub) window.aegisub.postMessage(message);
	}
	function editorFrame() {
		return document.getElementById("image-editor-frame");
	}
	function editorWindow() {
		var frame = editorFrame();
		return frame ? frame.contentWindow : null;
	}
	function postScript(script) {
		var editor = editorWindow();
		if (!editor) throw new Error("ImageEditor iframe is not available");
		editor.postMessage(script, "*");
	}
	function base64ToBuffer(value) {
		var binary = atob(value);
		var bytes = new Uint8Array(binary.length);
		for (var i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
		return bytes.buffer;
	}
	function base64ToText(value) {
		return new TextDecoder("utf-8").decode(new Uint8Array(base64ToBuffer(value)));
	}
	sceneLayerName = base64ToText(sceneLayerName);
	subtitleGroupName = base64ToText(subtitleGroupName);
	function bufferToBase64(buffer) {
		var bytes = buffer instanceof ArrayBuffer ? new Uint8Array(buffer) :
			new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength);
		var binary = "";
		for (var i = 0; i < bytes.length; i += 32768)
			binary += String.fromCharCode.apply(null, bytes.subarray(i, i + 32768));
		return btoa(binary);
	}
	function sceneScript(visible) {
		return "(function(){var d=app.activeDocument;if(!d)return;" +
			"var sceneName=" + JSON.stringify(sceneLayerName) + ";" +
			"for(var i=0;i<d.layers.length;i++){" +
			"if(d.layers[i].name===sceneName){" +
			"d.layers[i].visible=" + (visible ? "true" : "false") + ";break;}}})();";
	}
	window.__imageEditorOpenInput = function () {
		try {
			var editor = editorWindow();
			if (!editor) throw new Error("ImageEditor iframe is not available");
			var buffer = base64ToBuffer(window.__imageEditorInput);
			window.__imageEditorInput = "";
			inputSent = true;
			editor.postMessage(buffer, "*");
		}
		catch (error) { notify("error:" + error.message); }
	};
	function finishOrOpenNextSubtitle() {
		if (editorDocumentCount !== 1) {
			waitingForNextSubtitle = true;
			return;
		}
		waitingForNextSubtitle = false;
		if (subtitleIndex >= window.__imageEditorSubtitleInputs.length) {
			postScript("app.echoToOE('image-editor-subtitle-ready');");
			return;
		}
		try {
			var editor = editorWindow();
			if (!editor) throw new Error("ImageEditor iframe is not available");
			var buffer = base64ToBuffer(
				window.__imageEditorSubtitleInputs[subtitleIndex]);
			window.__imageEditorSubtitleInputs[subtitleIndex] = "";
			subtitleOpen = true;
			editor.postMessage(buffer, "*");
		}
		catch (error) { notify("error:" + error.message); }
	}
	window.__imageEditorSave = function () {
		try {
			if (saving) return;
			saving = true;
			postScript("(function(){var d=app.activeDocument;if(!d)throw new Error('No document');" +
				"var sceneName=" + JSON.stringify(sceneLayerName) + ";" +
				"var visible=-1;for(var i=0;i<d.layers.length;i++){" +
				"if(d.layers[i].name===sceneName){visible=d.layers[i].visible?1:0;break;}}" +
				"app.echoToOE('image-editor-scene-visible:'+visible);})();");
		}
		catch (error) {
			saving = false;
			notify("error:" + error.message);
		}
	};
	window.addEventListener("message", function (event) {
		if (event.source !== editorWindow()) return;
		if (event.data === "done") {
			notify("ready");
			return;
		}
		if (typeof event.data === "string" &&
			event.data.indexOf("image-editor-document-count:") === 0) {
			editorDocumentCount = parseInt(event.data.slice(28), 10) || 0;
			if (editorDocumentCount === 1 && !scenePrepared && inputSent) {
				scenePrepared = true;
				postScript("(function(){try{" +
					"var d=app.activeDocument;if(!d)throw new Error('No document');" +
					"var scene=d.activeLayer;scene.name=" +
					JSON.stringify(sceneLayerName) + ";scene.allLocked=true;" +
					"if(" + window.__imageEditorSubtitleInputs.length + ">1){" +
					"var group=d.layerSets.add();group.name=" +
					JSON.stringify(subtitleGroupName) + ";}" +
					"app.echoToOE('image-editor-scene-ready');" +
					"}catch(error){app.echoToOE('image-editor-error:'+" +
					"(error&&error.message?error.message:error));}})();");
			}
			else if (editorDocumentCount === 1 && waitingForNextSubtitle)
				finishOrOpenNextSubtitle();
			if (editorDocumentCount !== 2 || !subtitleOpen) return;

			subtitleOpen = false;
			var name = base64ToText(window.__imageEditorSubtitleNames[subtitleIndex]);
			var groupCount = window.__imageEditorSubtitleInputs.length;
			postScript("(function(){try{" +
				"if(app.documents.length<2)throw new Error('Subtitle document is missing');" +
				"var sub=app.activeDocument;var base=app.documents[0];" +
				"sub.activeLayer.duplicate(base,ElementPlacement.PLACEATBEGINNING);" +
				"sub.close(SaveOptions.DONOTSAVECHANGES);app.activeDocument=base;" +
				"var layer=base.activeLayer;" +
				"layer.name=" + JSON.stringify(name) + ";layer.rasterize();" +
				(groupCount > 1 ?
					"var group=base.layerSets.getByName(" +
					JSON.stringify(subtitleGroupName) + ");" +
					"layer.move(group,ElementPlacement.INSIDE);" : "") +
				"app.echoToOE('image-editor-subtitle-layer-ready:" + subtitleIndex + "');" +
				"}catch(error){app.echoToOE('image-editor-error:'+" +
				"(error&&error.message?error.message:error));}})();");
			return;
		}
		if (event.data === "image-editor-scene-ready") {
			finishOrOpenNextSubtitle();
			return;
		}
		if (typeof event.data === "string" &&
			event.data.indexOf("image-editor-subtitle-layer-ready:") === 0) {
			var completed = parseInt(event.data.slice(34), 10);
			if (completed === subtitleIndex) ++subtitleIndex;
			finishOrOpenNextSubtitle();
			return;
		}
		if (event.data === "image-editor-subtitle-ready") {
			notify("document-ready");
			return;
		}
		if (typeof event.data === "string" &&
			event.data.indexOf("image-editor-error:") === 0) {
			var wait = document.getElementById("image-editor-wait");
			if (wait) wait.remove();
			notify("error:" + event.data.slice(19));
			return;
		}
		if (typeof event.data === "string" &&
			event.data.indexOf("image-editor-scene-visible:") === 0) {
			sceneWasVisible = event.data.slice(27) === "1";
			postScript(sceneScript(false) +
				'app.activeDocument.saveToOE("png");');
			return;
		}
		if (!saving || !(event.data instanceof ArrayBuffer || ArrayBuffer.isView(event.data)))
			return;
		saving = false;
		try {
			var encoded = bufferToBase64(event.data);
			postScript(sceneScript(sceneWasVisible));
			var chunkSize = 262144;
			var count = Math.ceil(encoded.length / chunkSize);
			notify("png-start:" + count);
			for (var i = 0; i < count; i++)
				notify("png-chunk:" + i + ":" +
					encoded.slice(i * chunkSize, (i + 1) * chunkSize));
			notify("png-end");
		}
		catch (error) { notify("error:" + error.message); }
	});
})();)JS");
	auto encode_name = [](wxString const& name) {
		auto utf8 = from_wx(name);
		return wxBase64Encode(utf8.data(), utf8.size());
	};
	script.Replace("__IMAGE_EDITOR_SCENE_LAYER__", encode_name(_("Scene")));
	script.Replace("__IMAGE_EDITOR_SUBTITLE_GROUP__", encode_name(_("Subtitle")));
	return script;
}

class ImageEditorDialog final : public wxDialog {
	struct EncodedSubtitleLayer {
		wxString name;
		wxString png;
	};

	wxWebView *browser = nullptr;
	wxButton *insert = nullptr;
	wxStaticText *status = nullptr;
	wxString input;
	std::vector<EncodedSubtitleLayer> subtitle_inputs;
	wxImage output;
	std::vector<wxString> output_chunks;
	bool input_sent = false;
	bool usable = false;
	bool finishing = false;

	void ReportError(wxString const& message) {
		finishing = false;
		insert->Enable(input_sent);
		wxMessageBox(message, _("Image Editor"), wxOK | wxICON_WARNING, this);
	}

	void SendInput() {
		if (input_sent || input.empty()) return;
		input_sent = true;
		browser->RunScriptAsync("window.__imageEditorInput='';");
		constexpr size_t chunk_size = 262144;
		for (size_t offset = 0; offset < input.length(); offset += chunk_size) {
			wxString chunk = input.Mid(offset, chunk_size);
			browser->RunScriptAsync(
				"window.__imageEditorInput+='" + chunk + "';");
		}
		browser->RunScriptAsync(
			"window.__imageEditorSubtitleInputs=[];"
			"window.__imageEditorSubtitleNames=[];");
		for (size_t index = 0; index < subtitle_inputs.size(); ++index) {
			browser->RunScriptAsync(agi::wxformat(
				"window.__imageEditorSubtitleInputs[%zu]='';", index));
			for (size_t offset = 0; offset < subtitle_inputs[index].png.length();
				offset += chunk_size) {
				wxString chunk = subtitle_inputs[index].png.Mid(offset, chunk_size);
				browser->RunScriptAsync(agi::wxformat(
					"window.__imageEditorSubtitleInputs[%zu]+='%s';", index, chunk));
			}
			browser->RunScriptAsync(agi::wxformat(
				"window.__imageEditorSubtitleNames[%zu]='%s';", index,
				subtitle_inputs[index].name));
		}
		browser->RunScriptAsync("window.__imageEditorOpenInput();");
	}

	void FinishOutput() {
		if (finishing) return;
		finishing = true;
		if (output_chunks.empty() ||
			std::any_of(output_chunks.begin(), output_chunks.end(),
				[](wxString const& chunk) { return chunk.empty(); })) {
			ReportError(_("The edited image was incomplete."));
			return;
		}
		wxString encoded;
		for (auto const& chunk : output_chunks) encoded += chunk;
		wxMemoryBuffer decoded = wxBase64Decode(encoded);
		wxMemoryInputStream stream(decoded.GetData(), decoded.GetDataLen());
		if (!output.LoadFile(stream, wxBITMAP_TYPE_PNG)) {
			ReportError(_("The edited PNG could not be loaded."));
			return;
		}
		EndModal(wxID_OK);
	}

	void OnScriptMessage(wxWebViewEvent& event) {
		wxString message = event.GetString();
		if (message == "ready") {
			if (!input_sent) SendInput();
			return;
		}
		if (message == "document-ready") {
			if (input_sent) {
				status->SetLabel(_("Saving returns every visible canvas layer to the subtitle except the Scene layer."));
				status->GetParent()->Layout();
				insert->Enable();
			}
			return;
		}
		if (message.StartsWith("png-start:")) {
			long count = 0;
			finishing = false;
			if (message.Mid(10).ToLong(&count) && count > 0 && count < 10000)
				output_chunks.assign(static_cast<size_t>(count), wxString());
			return;
		}
		if (message.StartsWith("png-chunk:")) {
			wxString rest = message.Mid(10);
			int separator = rest.Find(':');
			long index = -1;
			if (separator != wxNOT_FOUND && rest.Left(separator).ToLong(&index) &&
				index >= 0 && static_cast<size_t>(index) < output_chunks.size())
				output_chunks[static_cast<size_t>(index)] =
					rest.Mid(separator + 1);
			return;
		}
		if (message == "png-end") {
			FinishOutput();
			return;
		}
		if (message.StartsWith("error:"))
			ReportError(message.Mid(6));
	}

public:
	ImageEditorDialog(wxWindow *parent, wxImage const& frame,
		std::vector<EditorImages::SubtitleLayer> const& subtitles)
	: wxDialog(parent, wxID_ANY, _("Image Editor"), wxDefaultPosition,
		wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX) {
		auto encode_png = [](wxImage const& image) {
			wxMemoryOutputStream png;
			if (!image.SaveFile(png, wxBITMAP_TYPE_PNG)) return wxString();
			auto *buffer = png.GetOutputStreamBuffer();
			return wxBase64Encode(buffer->GetBufferStart(), png.GetSize());
		};
		input = encode_png(frame);
		for (auto const& layer : subtitles) {
			auto name_utf8 = from_wx(layer.name);
			subtitle_inputs.push_back({
				wxBase64Encode(name_utf8.data(), name_utf8.size()),
				encode_png(layer.image)});
		}
		if (input.empty() || subtitle_inputs.empty() ||
			std::any_of(subtitle_inputs.begin(), subtitle_inputs.end(),
				[](EncodedSubtitleLayer const& layer) {
					return layer.name.empty() || layer.png.empty();
				})) return;

		auto root = new wxBoxSizer(wxVERTICAL);
		auto *resources = new ImageEditorResourceHandler;
		if (!resources->IsUsable()) {
			delete resources;
			return;
		}
		wxSharedPtr<wxWebViewHandler> resource_handler(resources);

#ifdef __WXMAC__
		// WKWebView requires custom scheme handlers to be registered on its
		// configuration before the native view is created.
		browser = wxWebView::New(BrowserBackend());
		if (!browser) return;
		browser->RegisterHandler(resource_handler);
		if (!browser->Create(this, wxID_ANY, "about:blank", wxDefaultPosition,
			wxDefaultSize, wxBORDER_NONE)) {
			delete browser;
			browser = nullptr;
			return;
		}
#else
		browser = wxWebView::New(this, wxID_ANY, "about:blank",
			wxDefaultPosition, wxDefaultSize, BrowserBackend(), wxBORDER_NONE);
		if (!browser) return;
		browser->RegisterHandler(resource_handler);
#endif
		browser->EnableContextMenu(false);
		if (!browser->AddScriptMessageHandler("aegisub") ||
			!browser->AddUserScript(BridgeScript())) return;
		browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED,
			&ImageEditorDialog::OnScriptMessage, this);
		browser->Bind(wxEVT_WEBVIEW_ERROR, [this](wxWebViewEvent&) {
			ReportError(_("ImageEditor could not be loaded."));
		});
		root->Add(browser, 1, wxEXPAND);

		auto bottom = new wxBoxSizer(wxHORIZONTAL);
		status = new wxStaticText(this, wxID_ANY, _("Loading..."));
		insert = new wxButton(this, wxID_ANY, _("Save"));
		insert->Disable();
		auto cancel = new wxButton(this, wxID_CANCEL, _("Cancel"));
		bottom->Add(status, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
		bottom->AddStretchSpacer();
		bottom->Add(insert, 0, wxRIGHT, FromDIP(6));
		bottom->Add(cancel, 0);
		root->Add(bottom, 0, wxEXPAND | wxALL, FromDIP(8));
		SetSizer(root);

		int display_index = wxDisplay::GetFromWindow(parent);
		if (display_index == wxNOT_FOUND) display_index = 0;
		wxRect display_area = wxDisplay(display_index).GetClientArea();
		wxSize available = display_area.GetSize();
		wxSize desired(std::min(FromDIP(1920), available.x),
			std::min(FromDIP(1080), available.y));
		SetMaxSize(available);
		SetMinSize(wxSize(std::min(FromDIP(1100), desired.x),
			std::min(FromDIP(700), desired.y)));
		SetSize(desired);
		Move(display_area.x + (available.x - desired.x) / 2,
			display_area.y + (available.y - desired.y) / 2);

		insert->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			insert->Disable();
			finishing = false;
			output_chunks.clear();
			browser->RunScriptAsync("window.__imageEditorSave();");
		});
		usable = true;
		browser->LoadURL(EditorUrl());
	}

	bool IsUsable() const { return usable; }
	wxImage TakeOutput() { return std::move(output); }
};

#endif

} // namespace

void EditWithImageEditor(agi::Context *context) {
	if (glitch::SelectionHasEnabledAnimation(context)) {
		wxMessageBox(_("A glitch effect with animation cannot be edited."),
			_("Image Editor"), wxOK | wxICON_WARNING, context->parent);
		return;
	}
	auto images = CaptureEditorImages(context);
	if (!images) return;

#if wxUSE_WEBVIEW
	wxString backend = BrowserBackend();
	if (wxWebView::IsBackendAvailable(backend)) {
		ImageEditorDialog dialog(context->parent, images->frame, images->subtitles);
		if (dialog.IsUsable()) {
			if (dialog.ShowModal() == wxID_OK)
				InsertEditedImage(context, dialog.TakeOutput());
			return;
		}
	}
#endif

	wxMessageBox(
		_("ImageEditor requires embedded web-view support, which is unavailable."),
		_("Image Editor"), wxOK | wxICON_WARNING, context->parent);
}

} // namespace typesetting::image_insert
