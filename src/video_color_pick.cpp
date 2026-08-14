// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file video_color_pick.cpp
/// @brief Taking a colour off the video under the pointer
///
/// Grown out of an Automation script that did the same job through
/// aegisub.get_cursor_position and aegisub.get_frame. Two things are different
/// here. The script fed script coordinates straight into a frame buffer index,
/// which only lands on the right pixel while the script resolution and the video
/// resolution agree; this scales between them. And the tag goes in through the
/// same code the colour and font pickers use, so a selection of several lines,
/// the caret position and the undo entry all behave the way they do there.

#include "video_color_pick.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_parsed_line.h"
#include "compat.h"
#include "dialogs.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "text_selection_controller.h"
#include "utils.h"
#include "vector2d.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_frame.h"

#include <libaegisub/color.h>
#include <libaegisub/format.h>
#include <libaegisub/signal.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dcbuffer.h>
#include <wx/msgdlg.h>
#include <wx/popupwin.h>
#include <wx/progdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>

namespace video_color_pick {
namespace {

/// The tag a target writes, and the spelling of the same tag that a line may
/// already be using. Only the primary colour has two spellings.
struct TagNames {
	const char *tag;
	const char *alt;
};

TagNames NamesFor(Target target) {
	switch (target) {
	case Target::Outline: return {"\\3c", ""};
	case Target::Shadow:  return {"\\4c", ""};
	default:              return {"\\c", "\\1c"};
	}
}

/// Below this the transition would start at or before the line does, which is the
/// same as not having one. The script this grew from used the same threshold.
constexpr int MINIMUM_TRANSITION_MS = 30;

/// The pick is taken to belong to the middle of the displayed frame rather than to
/// its first millisecond, and lands a hair before it, so that the colour is already
/// in place when the frame is shown.
constexpr int TRANSITION_LEAD_MS = 15;

/// A second press of the same key inside this many milliseconds is the gesture that
/// opens the magnifier. Short enough not to catch someone picking two colours in a
/// row, long enough to be comfortable to perform on purpose.
constexpr int DOUBLE_PRESS_MS = 200;

/// The magnified view: how much of the video it shows, and how big each of those
/// pixels is drawn. Odd counts so that there is a middle pixel to aim at.
constexpr int MAGNIFIER_COLUMNS = 17;
constexpr int MAGNIFIER_ROWS = 13;
constexpr int MAGNIFIER_SCALE = 12;

// ---------------------------------------------------------------- frame sampling

int FrameMidpoint(const agi::Context *c) {
	int frame = c->videoController->GetFrameN();
	return (c->videoController->TimeAtFrame(frame, agi::vfr::START) +
		c->videoController->TimeAtFrame(frame + 1, agi::vfr::START)) / 2;
}

/// One frame, with the addressing arithmetic in one place.
class FrameSampler {
	std::shared_ptr<VideoFrame> frame;

public:
	explicit FrameSampler(const agi::Context *c)
	: frame(c && c->project->VideoProvider()
		? c->videoController->GetFrame(c->videoController->GetFrameN(), true)
		: nullptr) { }

	bool ok() const { return frame && frame->width > 0 && frame->height > 0; }
	int width() const { return ok() ? (int)frame->width : 0; }
	int height() const { return ok() ? (int)frame->height : 0; }

	std::optional<agi::Color> At(int x, int y) const {
		if (!ok() || x < 0 || y < 0 || x >= width() || y >= height())
			return {};
		if (frame->flipped)
			y = height() - 1 - y;
		// VideoFrame is BGRA; agi::Color takes RGB.
		size_t at = (size_t)y * frame->pitch + (size_t)x * 4;
		return agi::Color(frame->data[at + 2], frame->data[at + 1], frame->data[at]);
	}
};

/// Where the pointer is, in video pixels.
///
/// GetMousePosition is in script coordinates - it has already been through the
/// visual tool's pan and zoom - while a frame is indexed in video pixels, so the
/// two resolutions have to be reconciled. The script this grew from skipped that
/// step, which only agreed with reality while the two were the same size.
std::optional<wxPoint> PointerInFrame(const agi::Context *c, FrameSampler const& frame) {
	if (!c->videoDisplay || !frame.ok())
		return {};

	Vector2D point = c->videoDisplay->GetMousePosition();
	if (!point)
		return {};

	int script_w = 0, script_h = 0;
	c->ass->GetResolution(script_w, script_h);
	if (script_w <= 0 || script_h <= 0)
		return {};

	wxPoint at((int)(point.X() * frame.width() / script_w),
	           (int)(point.Y() * frame.height() / script_h));
	if (at.x < 0 || at.y < 0 || at.x >= frame.width() || at.y >= frame.height())
		return {};
	return at;
}

// -------------------------------------------------------------- writing the tag

/// Whether a tag name is one of the colours these targets write.
bool IsColourOf(std::string const& name, std::vector<Target> const& targets) {
	for (Target target : targets) {
		TagNames names = NamesFor(target);
		if (name == names.tag || (*names.alt && name == names.alt))
			return true;
	}
	return false;
}

/// Whether this \t tag is one of ours: a transition that carries nothing but colours,
/// at least one of which is a colour we are about to write.
///
/// "Nothing but colours" is what keeps it off a transition that also moves or fades
/// something - those are somebody else's and are left alone. Colours of targets other
/// than ours inside the same transition do go, because the writes here put every
/// chosen colour into one transition and replacing it wholesale is the only way that
/// stays predictable.
bool IsOurTransition(AssOverrideTag const& tag, std::vector<Target> const& targets) {
	if (tag.Name != "\\t" || tag.Params.empty())
		return false;

	auto const& body = tag.Params.back();
	if (body.omitted || body.GetType() != VariableDataType::BLOCK)
		return false;

	auto block = body.Get<AssDialogueBlockOverride*>();
	if (!block || block->Tags.empty())
		return false;

	bool ours = false;
	for (auto const& inner : block->Tags) {
		// Every colour there is, so a merged transition is recognised whichever of
		// the three it happens to hold.
		if (!IsColourOf(inner.Name, {Target::Primary, Target::Outline, Target::Shadow}))
			return false;
		if (IsColourOf(inner.Name, targets))
			ours = true;
	}
	return ours;
}

/// The body of a transition that sets every one of these targets to one colour:
/// "\c&H..&\3c&H..&". One transition for the lot rather than one each, because they
/// all happen at the same moment and reading three of them in a row says nothing the
/// one does not.
std::string TransitionBody(std::vector<Target> const& targets, std::string const& value) {
	std::string body;
	for (Target target : targets)
		body += NamesFor(target).tag + value;
	return body;
}

/// Write the colour as a transition rather than as a plain tag.
///
/// parsed_line::set_tag cannot express this: it replaces a tag of one name, and a
/// transition is a \t whose body happens to contain that tag. So the transitions
/// that would fight with this one are removed and a fresh one appended.
void SetTransition(parsed_line& parsed, std::vector<Target> const& targets,
                   std::string const& value, int time, int norm_pos, int orig_pos) {
	std::string insert = agi::format("\\t(%d,%d,%s)", time, time,
		TransitionBody(targets, value));

	int blockn = parsed.block_at_pos(norm_pos);
	AssDialogueBlockOverride *ovr = nullptr;
	while (blockn >= 0 && !ovr) {
		AssDialogueBlock *block = parsed.blocks[blockn].get();
		if (block->GetType() == AssBlockType::OVERRIDE)
			ovr = static_cast<AssDialogueBlockOverride*>(block);
		else if (block->GetType() == AssBlockType::PLAIN)
			break;
		else
			--blockn;
	}

	if (!ovr) {
		std::string_view text = parsed.line->Text.get();
		if (orig_pos < 0 || (size_t)orig_pos > text.size())
			orig_pos = 0;
		parsed.line->Text = agi::Str(text.substr(0, orig_pos), "{", insert, "}",
			text.substr(orig_pos));
		parsed.blocks = parsed.line->ParseTags();
		return;
	}

	ovr->Tags.erase(
		std::remove_if(ovr->Tags.begin(), ovr->Tags.end(),
			[&](AssOverrideTag const& tag) { return IsOurTransition(tag, targets); }),
		ovr->Tags.end());
	ovr->AddTag(insert);
	parsed.line->UpdateText(parsed.blocks);
}

/// How long into the active line the current frame is, which is when a transition
/// would be timed to. Negative or tiny values mean a transition makes no sense.
int TransitionTime(const agi::Context *c) {
	const auto line = c->selectionController->GetActiveLine();
	if (!line) return 0;
	return FrameMidpoint(c) - line->Start - TRANSITION_LEAD_MS;
}

/// Write one colour into every selected line, as every one of these targets, and
/// return the undo entry's id.
///
/// `commit_id` carries an earlier entry to amend rather than pile onto: a pick
/// followed by the magnifier is one action from the user's point of view, and should
/// be one step to undo.
int Apply(agi::Context *c, std::vector<Target> const& targets, agi::Color colour,
          bool transition, int commit_id) {
	std::string value = colour.GetAssOverrideFormatted();
	int midpoint = FrameMidpoint(c);

	auto const& sel = c->selectionController->GetSelectedSet();

	// Where the tag goes. The caret belongs to the active line, so it only means
	// anything when that is the only line being written to; with several selected it
	// would be pointing into one of them at an offset the others know nothing about,
	// so they all get it at the front. The caret is also ignored when it sits past
	// the end of the line, which happens after the text has been edited elsewhere.
	const auto active_line = c->selectionController->GetActiveLine();
	int caret = 0;
	if (sel.size() == 1) {
		caret = c->textSelectionController->GetSelectionStart();
		if (caret < 0 || (size_t)caret > active_line->Text.get().size())
			caret = 0;
	}
	const int norm_caret = normalize_pos(active_line->Text, caret);

	for (auto line : sel) {
		parsed_line parsed(line);
		int time = midpoint - line->Start - TRANSITION_LEAD_MS;

		if (transition && time > MINIMUM_TRANSITION_MS) {
			SetTransition(parsed, targets, value, time, norm_caret, caret);
		}
		else {
			// Plain tags do not merge - they are separate tags in the same block,
			// which is how a line normally reads.
			int shift = 0;
			for (Target target : targets)
				shift += parsed.set_tag(NamesFor(target).tag, value, norm_caret,
					caret + shift);
		}
	}

	int new_commit_id = c->ass->Commit(_("pick color from video"), AssFile::COMMIT_DIAG_TEXT,
		commit_id, sel.size() == 1 ? *sel.begin() : nullptr);
	AddColorToRecent(colour);
	return new_commit_id;
}

// --------------------------------------------------------------- motion tracks

std::string Trim(std::string value) {
	auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

/// A tracked position per frame, as After Effects writes it and as Aegisub-Motion
/// reads it off the clipboard.
struct MotionTrack {
	int source_width = 0;
	int source_height = 0;
	std::vector<std::pair<double, double>> position;
};

/// Parse the clipboard's idea of a motion track.
///
/// The format is what Mocha exports and what a-mo/DataHandler.moon accepts: a header
/// naming the source size, then a "Position" section whose rows are indented and
/// start with a frame number. Only the position is wanted here - scale and rotation
/// say nothing about colour - so the other sections are skipped rather than parsed.
std::optional<MotionTrack> ParseMotionTrack(std::string const& text) {
	if (text.find("Adobe After Effects 6.0 Keyframe Data") == std::string::npos)
		return {};

	MotionTrack track;
	bool in_position = false;

	std::istringstream input(text);
	std::string line;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;

		bool indented = line.front() == '\t' || line.front() == ' ';
		if (!indented) {
			// A section header. Anything that is not Position turns collection off,
			// so rows belonging to Scale or Rotation cannot be mistaken for it.
			in_position = Trim(line) == "Position";
			continue;
		}

		std::string body = Trim(line);
		if (!track.source_width) {
			int value = 0;
			if (sscanf(body.c_str(), "Source Width %d", &value) == 1) {
				track.source_width = value;
				continue;
			}
		}
		if (!track.source_height) {
			int value = 0;
			if (sscanf(body.c_str(), "Source Height %d", &value) == 1) {
				track.source_height = value;
				continue;
			}
		}
		if (!in_position) continue;

		// "<frame> <x> <y> [<z>]". The header row of the section names its columns
		// instead, and fails to parse, which is how it gets skipped.
		double frame = 0, x = 0, y = 0;
		if (sscanf(body.c_str(), "%lf %lf %lf", &frame, &x, &y) == 3)
			track.position.emplace_back(x, y);
	}

	if (track.source_width <= 0 || track.source_height <= 0 || track.position.empty())
		return {};
	return track;
}

/// Replace this target's colour on the line with a whole timed sequence of them.
///
/// The first sample belongs to the line's own first frame, so it goes in as a plain
/// tag; everything after it is a \t() at the offset the frame falls on. Existing
/// colours of this target, plain or transitioned, are cleared first so that running
/// the import twice does not stack two sequences on top of each other.
void ApplyTrackedColours(agi::Context *c, AssDialogue *line,
                         std::vector<Target> const& targets,
                         std::vector<std::pair<int, agi::Color>> const& stops) {
	std::vector<std::string> insert;
	for (auto const& [time, colour] : stops) {
		std::string value = colour.GetAssOverrideFormatted();
		// Every target in one transition per moment. Writing one transition each
		// gave \t(150,150,\c&H..&) and \t(150,150,\3c&H..&) side by side, saying the
		// same thing twice at the same time.
		if (time <= MINIMUM_TRANSITION_MS)
			insert.push_back(TransitionBody(targets, value));
		else
			insert.push_back(agi::format("\\t(%d,%d,%s)", time, time,
				TransitionBody(targets, value)));
	}
	if (insert.empty())
		return;

	parsed_line parsed(line);
	int blockn = parsed.block_at_pos(0);
	AssDialogueBlockOverride *ovr = nullptr;
	while (blockn >= 0 && !ovr) {
		AssDialogueBlock *block = parsed.blocks[blockn].get();
		if (block->GetType() == AssBlockType::OVERRIDE)
			ovr = static_cast<AssDialogueBlockOverride*>(block);
		else if (block->GetType() == AssBlockType::PLAIN)
			break;
		else
			--blockn;
	}

	if (!ovr) {
		std::string all;
		for (auto const& tag : insert) all += tag;
		parsed.line->Text = agi::Str("{", all, "}", parsed.line->Text.get());
		return;
	}

	ovr->Tags.erase(
		std::remove_if(ovr->Tags.begin(), ovr->Tags.end(),
			[&](AssOverrideTag const& tag) {
				return IsOurTransition(tag, targets) || IsColourOf(tag.Name, targets);
			}),
		ovr->Tags.end());
	for (auto const& tag : insert)
		ovr->AddTag(tag);
	parsed.line->UpdateText(parsed.blocks);
}

/// Whether the line already carries timed colours for any of these targets - an
/// earlier import, or a pick made with the transition option on. The import clears
/// them, so it has to say so before it does.
bool HasTimedColours(AssDialogue *line, std::vector<Target> const& targets) {
	parsed_line parsed(line);
	for (auto& block : parsed.blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		auto ovr = static_cast<AssDialogueBlockOverride*>(block.get());
		for (auto const& tag : ovr->Tags)
			if (IsOurTransition(tag, targets))
				return true;
	}
	return false;
}

/// Follow a tracked point through the active line and write the colour it passes
/// over. Returns false only when the clipboard has nothing usable on it.
bool ImportFromMotion(agi::Context *c, std::vector<Target> const& targets,
                      wxWindow *parent) {
	auto track = ParseMotionTrack(GetClipboard());
	if (!track)
		return false;

	AssDialogue *line = c->selectionController->GetActiveLine();
	if (!line) return true;

	// Asked before the work rather than after it: the reading below decodes a frame
	// per sample, and there is no point spending that to then be told no.
	if (HasTimedColours(line, targets) &&
		wxMessageBox(
			_("This line already has timed colors for that tag, from an earlier import "
			  "or from a pick made with the transition option on.\n"
			  "Importing replaces them. Do you want to continue?"),
			_("Import from motion"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
			parent) != wxYES)
		return true;

	int first_frame = c->videoController->FrameAtTime(line->Start, agi::vfr::START);
	int last_frame = c->videoController->FrameAtTime(line->End, agi::vfr::START);
	int count = std::min<int>((int)track->position.size(), last_frame - first_frame + 1);
	if (count <= 0) return true;

	// Every frame has to be decoded to be sampled, so a long line is a real wait.
	std::unique_ptr<wxProgressDialog> progress;
	if (count > 40)
		progress = std::make_unique<wxProgressDialog>(_("Import from motion"),
			_("Reading the colour along the track..."), count, parent,
			wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT);

	std::vector<std::pair<int, agi::Color>> stops;
	for (int index = 0; index < count; ++index) {
		if (progress && !progress->Update(index))
			break;

		int frame = first_frame + index;
		// Re-fetching per frame is what makes this slow, and there is no way round
		// it: the colour under the point is a property of that frame.
		auto sampled = [&]() -> std::optional<agi::Color> {
			auto shot = c->videoController->GetFrame(frame, true);
			if (!shot || shot->width == 0) return {};
			int x = (int)(track->position[index].first * shot->width / track->source_width);
			int y = (int)(track->position[index].second * shot->height / track->source_height);
			if (x < 0 || y < 0 || (size_t)x >= shot->width || (size_t)y >= shot->height)
				return {};
			if (shot->flipped) y = (int)shot->height - 1 - y;
			size_t at = (size_t)y * shot->pitch + (size_t)x * 4;
			return agi::Color(shot->data[at + 2], shot->data[at + 1], shot->data[at]);
		}();
		if (!sampled) continue;

		// One tag per change rather than one per frame. A tracked point usually
		// holds its colour for a while, and writing every frame would bury the line
		// in tags that all say the same thing.
		if (!stops.empty() && stops.back().second == *sampled)
			continue;

		int time = c->videoController->TimeAtFrame(frame, agi::vfr::START) - line->Start;
		stops.emplace_back(time, *sampled);
	}

	progress.reset();
	if (stops.empty())
		return true;

	ApplyTrackedColours(c, line, targets, stops);
	c->ass->Commit(_("import colors from motion"), AssFile::COMMIT_DIAG_TEXT, -1, line);
	return true;
}

// ------------------------------------------------------------------- magnifier

/// The magnified pixels. Drawn from the frame rather than from the screen, so it is
/// sharp whatever the video is zoomed to, and shows what will actually be sampled
/// rather than what the display happens to have scaled.
class MagnifierCanvas final : public wxWindow {
	agi::Context *c;
	wxPoint centre;
	/// Which pixel a click has settled on. Starts on the one the gesture began at,
	/// so doing nothing but double-clicking takes the same colour a single press
	/// would have.
	wxPoint chosen;
	std::function<void (agi::Color)> onAccept;

public:
	MagnifierCanvas(wxWindow *parent, agi::Context *c, wxPoint centre,
	                std::function<void (agi::Color)> onAccept)
	: wxWindow(parent, wxID_ANY, wxDefaultPosition,
		wxSize(MAGNIFIER_COLUMNS * MAGNIFIER_SCALE, MAGNIFIER_ROWS * MAGNIFIER_SCALE))
	, c(c), centre(centre), chosen(centre), onAccept(std::move(onAccept))
	{
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		Bind(wxEVT_PAINT, &MagnifierCanvas::OnPaint, this);
		// A single click only marks a pixel; the colour is taken on a double click.
		// Otherwise the first click would close the popup, and there would be no way
		// to pick a colour and then reach the motion import in the same visit.
		Bind(wxEVT_LEFT_DOWN, &MagnifierCanvas::OnSelect, this);
		Bind(wxEVT_LEFT_DCLICK, &MagnifierCanvas::OnAccept, this);
	}

	/// The frame moved under us, so the pixels did too.
	void Reread() { Refresh(); }

private:
	/// Which video pixel a point in this window stands for.
	wxPoint SourceAt(wxPoint at) const {
		return wxPoint(centre.x - MAGNIFIER_COLUMNS / 2 + at.x / MAGNIFIER_SCALE,
		               centre.y - MAGNIFIER_ROWS / 2 + at.y / MAGNIFIER_SCALE);
	}

	void OnPaint(wxPaintEvent&) {
		wxAutoBufferedPaintDC dc(this);
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE)));
		dc.DrawRectangle(GetClientRect());

		FrameSampler frame(c);
		if (!frame.ok()) return;

		for (int row = 0; row < MAGNIFIER_ROWS; ++row) {
			for (int column = 0; column < MAGNIFIER_COLUMNS; ++column) {
				auto colour = frame.At(centre.x - MAGNIFIER_COLUMNS / 2 + column,
				                       centre.y - MAGNIFIER_ROWS / 2 + row);
				if (!colour) continue;
				dc.SetBrush(wxBrush(wxColour(colour->r, colour->g, colour->b)));
				dc.DrawRectangle(column * MAGNIFIER_SCALE, row * MAGNIFIER_SCALE,
					MAGNIFIER_SCALE, MAGNIFIER_SCALE);
			}
		}

		// One marker, on the pixel that would be taken. It starts on the pixel the
		// gesture began at and moves with the clicks; drawing where it started as
		// well only invites reading the wrong one.
		dc.SetBrush(*wxTRANSPARENT_BRUSH);
		DrawMarker(dc, chosen, 2);
	}

	void DrawMarker(wxDC& dc, wxPoint source, int thickness) {
		int x = (source.x - centre.x + MAGNIFIER_COLUMNS / 2) * MAGNIFIER_SCALE;
		int y = (source.y - centre.y + MAGNIFIER_ROWS / 2) * MAGNIFIER_SCALE;
		for (int ring = 0; ring < thickness; ++ring) {
			dc.SetPen(wxPen(ring % 2 ? *wxBLACK : *wxWHITE, 1));
			dc.DrawRectangle(x - ring, y - ring,
				MAGNIFIER_SCALE + 2 * ring, MAGNIFIER_SCALE + 2 * ring);
		}
		dc.SetPen(wxPen(thickness > 1 ? *wxBLACK : *wxLIGHT_GREY, 1));
		dc.DrawRectangle(x - thickness, y - thickness,
			MAGNIFIER_SCALE + 2 * thickness, MAGNIFIER_SCALE + 2 * thickness);
	}

	void OnSelect(wxMouseEvent& evt) {
		chosen = SourceAt(evt.GetPosition());
		Refresh();
	}

	void OnAccept(wxMouseEvent& evt) {
		chosen = SourceAt(evt.GetPosition());
		FrameSampler frame(c);
		if (auto colour = frame.At(chosen.x, chosen.y))
			onAccept(*colour);
	}
};

/// The magnified view with its two choices above it.
///
/// A transient popup so that clicking anywhere else puts it away, and a timer that
/// puts it away when the pointer wanders off it - which is the gesture for "never
/// mind" and is what the script's users are used to from the built-in eyedropper.
class MagnifierPopup final : public wxPopupTransientWindow {
	agi::Context *c;
	Target target;
	int commit_id;

	wxCheckBox *transition;
	wxCheckBox *remember;
	/// The two colours this press is not for, so one pick can set more than one.
	std::vector<std::pair<Target, wxCheckBox*>> also;
	MagnifierCanvas *canvas;
	wxTimer leave_timer;
	agi::signal::Connection seek_connection;

public:
	MagnifierPopup(agi::Context *c, Target target, wxPoint centre, int commit_id)
	: wxPopupTransientWindow(c->videoDisplay, wxBORDER_SIMPLE)
	, c(c), target(target), commit_id(commit_id)
	{
		transition = new wxCheckBox(this, wxID_ANY, "");
		transition->SetValue(OPT_GET("Video/Color Pick/Auto Transition")->GetBool());

		remember = new wxCheckBox(this, wxID_ANY, _("remember for future use"));

		auto *also_row = new wxBoxSizer(wxHORIZONTAL);
		also_row->Add(new wxStaticText(this, wxID_ANY, _("also add for:")), 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		for (Target other : {Target::Primary, Target::Outline, Target::Shadow}) {
			// The colour that was asked for is being written anyway, so offering it
			// here would only be a checkbox that cannot mean anything.
			if (other == target) continue;
			auto *box = new wxCheckBox(this, wxID_ANY, NamesFor(other).tag);
			also_row->Add(box, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
			also.emplace_back(other, box);
		}

		canvas = new MagnifierCanvas(this, c, centre,
			[this](agi::Color colour) { Chose(colour); });

		auto *import = new wxButton(this, wxID_ANY, _("import from motion"));
		import->Bind(wxEVT_BUTTON, &MagnifierPopup::OnImport, this);

		// Says what the click does, because a single click no longer finishes and
		// there would otherwise be nothing to suggest a second one.
		auto *hint = new wxStaticText(this, wxID_ANY, _("accept color by doubleclick"));
		wxFont hint_font = hint->GetFont();
		hint_font.SetStyle(wxFONTSTYLE_ITALIC);
		hint->SetFont(hint_font);
		hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));

		auto *sizer = new wxBoxSizer(wxVERTICAL);
		sizer->Add(transition, 0, wxLEFT | wxRIGHT | wxTOP, 6);
		// Indented, because it is not a choice of its own: it says whether the one
		// above it should also apply to the presses after this one.
		sizer->Add(remember, 0, wxLEFT | wxRIGHT, 22);
		sizer->Add(also_row, 0, wxLEFT | wxRIGHT | wxTOP, 6);
		sizer->Add(canvas, 0, wxALL, 6);
		sizer->Add(import, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);
		sizer->Add(hint, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 4);
		SetSizerAndFit(sizer);

		UpdateTransitionLabel();

		leave_timer.SetOwner(this);
		Bind(wxEVT_TIMER, &MagnifierPopup::OnLeaveCheck, this);
		leave_timer.Start(120);

		// The label names the time the transition would be written with, and that
		// time is a property of the frame on screen, so stepping the video has to
		// rewrite it - and redraw the pixels, which have also just changed.
		seek_connection = c->videoController->AddSeekListener(&MagnifierPopup::OnSeek, this);
	}

private:
	void OnSeek(int) {
		UpdateTransitionLabel();
		canvas->Reread();
	}

	void UpdateTransitionLabel() {
		int time = TransitionTime(c);
		// The tag already carries its own "c" - \c, \3c, \4c - so the format must not
		// add another one.
		transition->SetLabel(wxString::Format(_("add with \\t(%d,%d,%s...)"),
			time, time, NamesFor(target).tag));

		// Deliberately never disabled. Below the threshold a transition would start
		// where the line does and so does nothing, and the write falls back to a
		// plain tag on its own - but greying the boxes out for that made them look
		// broken, and "remember" is about later presses anyway, where the time will
		// usually be a different one.

		GetSizer()->SetSizeHints(this);
		Layout();
	}

	/// Away from the popup means cancel. Polling rather than watching for a leave
	/// event, because a leave fires when the pointer crosses onto one of the child
	/// controls too, and that is not leaving.
	void OnLeaveCheck(wxTimerEvent&) {
		if (!GetScreenRect().Inflate(4).Contains(wxGetMousePosition()))
			Dismiss();
	}

	/// The colour asked for, plus any of the other two that were ticked.
	std::vector<Target> ChosenTargets() const {
		std::vector<Target> targets{target};
		for (auto const& [other, box] : also)
			if (box->GetValue()) targets.push_back(other);
		return targets;
	}

	void Chose(agi::Color colour) {
		bool wants = transition->GetValue();
		if (remember->GetValue())
			OPT_SET("Video/Color Pick/Auto Transition")->SetBool(wants);

		// One undo entry for the lot: the same id is carried through, so writing
		// three colours from one click stays one step to undo.
		Apply(c, ChosenTargets(), colour, wants, commit_id);
		Dismiss();
	}

	void OnImport(wxCommandEvent&) {
		// The popup goes first, and the dialogs below are parented to the main window
		// rather than to it. A modal dialog owned by a transient popup leaves the
		// focus somewhere else entirely when it closes - another application, in
		// practice - because the popup it belonged to is already on its way out.
		auto targets = ChosenTargets();
		wxWindow *owner = c->parent;
		Dismiss();

		if (!ImportFromMotion(c, targets, owner))
			wxMessageBox(_("There is no valid motion track on the clipboard."),
				_("Import from motion"), wxOK | wxICON_EXCLAMATION, owner);
	}

	void OnDismiss() override {
		leave_timer.Stop();
		seek_connection.Disconnect();
		CallAfter([this] { Destroy(); });
	}
};

// ------------------------------------------------------------------- the gesture

/// What the last press was, so that a second one can be recognised as a gesture
/// rather than as another pick. Static because the presses arrive as separate
/// command invocations with nothing of their own to remember between them.
struct LastPress {
	wxMilliClock_t when = 0;
	Target target = Target::Primary;
	int commit_id = -1;
	bool valid = false;
};
LastPress last_press;

} // namespace

bool CanPick(const agi::Context *c) {
	return c && c->project->VideoProvider() && c->videoDisplay &&
		c->selectionController->GetActiveLine() &&
		!c->selectionController->GetSelectedSet().empty();
}

void Invoke(agi::Context *c, Target target) {
	if (!CanPick(c))
		return;

	FrameSampler frame(c);
	auto at = PointerInFrame(c, frame);
	if (!at)
		return;

	wxMilliClock_t now = wxGetLocalTimeMillis();
	bool second = last_press.valid && last_press.target == target &&
		now - last_press.when < DOUBLE_PRESS_MS;

	if (second) {
		// The first press has already written a colour. The magnifier amends that
		// same undo entry rather than adding another, so the pair reads as the one
		// action it was, and whatever is chosen here replaces what was taken there.
		last_press.valid = false;
		auto *popup = new MagnifierPopup(c, target, *at, last_press.commit_id);
		wxPoint mouse = wxGetMousePosition();
		wxSize size = popup->GetSize();
		popup->Position(wxPoint(mouse.x - size.GetWidth() / 2,
			mouse.y - size.GetHeight() / 2), wxSize(0, 0));
		popup->Popup();
		return;
	}

	auto colour = frame.At(at->x, at->y);
	if (!colour)
		return;

	last_press.when = now;
	last_press.target = target;
	last_press.commit_id = Apply(c, {target}, *colour,
		OPT_GET("Video/Color Pick/Auto Transition")->GetBool(), -1);
	last_press.valid = true;
}

} // namespace video_color_pick
