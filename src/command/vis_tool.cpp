// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

#include "command.h"

#include "../include/aegisub/context.h"
#include "../image_mask_combiner.h"
#include "../libresrc/libresrc.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../video_display.h"
#include "../visual_tool_clip.h"
#include "../visual_tool_cross.h"
#include "../visual_tool_drag.h"
#include "../visual_tool_mask.h"
#include "../visual_tool_shape.h"
#include "../visual_tool_textbox.h"
#include "../visual_tool_perspective.h"
#include "../visual_tool_rotatexy.h"
#include "../visual_tool_rotatez.h"
#include "../visual_tool_scale.h"
#include "../visual_tool_vector_clip.h"

#include <algorithm>

#include <wx/image.h>
#include <wx/settings.h>

namespace {
	using cmd::Command;

	wxBitmap MakeTextboxBitmap(int requested_size) {
		int size = std::max(requested_size, 8);
		wxImage image(size, size, true);
		image.InitAlpha();
		std::fill(image.GetAlpha(), image.GetAlpha() + size * size, 0);
		wxColour colour = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
		auto set_pixel = [&](int x, int y) {
			if (x < 0 || y < 0 || x >= size || y >= size) return;
			image.SetRGB(x, y, colour.Red(), colour.Green(), colour.Blue());
			image.SetAlpha(x, y, 255);
		};

		int margin = std::max(2, size / 5);
		int last = size - margin - 1;
		int dash = std::max(2, size / 6);
		int gap = std::max(1, size / 10);
		int thickness = std::max(1, size / 16);
		for (int at = margin; at <= last; at += dash + gap) {
			for (int along = at; along <= std::min(last, at + dash - 1); ++along) {
				for (int stroke = 0; stroke < thickness; ++stroke) {
					set_pixel(along, margin + stroke);
					set_pixel(along, last - stroke);
					set_pixel(margin + stroke, along);
					set_pixel(last - stroke, along);
				}
			}
		}
		int line_left = margin + std::max(2, size / 8);
		int line_right = last - std::max(2, size / 8);
		for (int y : {margin + (last - margin) * 2 / 5, margin + (last - margin) * 3 / 5})
			for (int x = line_left; x <= line_right; ++x)
				for (int stroke = 0; stroke < thickness; ++stroke)
					set_pixel(x, y + stroke);
		return wxBitmap(image);
	}

	std::string CurrentVisualTool(const agi::Context *c) {
		if (c->videoDisplay->ToolIsType(typeid(VisualToolDrag))) return "video/tool/drag";
		if (c->videoDisplay->ToolIsType(typeid(VisualToolRotateZ))) return "video/tool/rotate/z";
		if (c->videoDisplay->ToolIsType(typeid(VisualToolRotateXY))) return "video/tool/rotate/xy";
		if (c->videoDisplay->ToolIsType(typeid(VisualToolPerspective))) return "video/tool/perspective";
		if (c->videoDisplay->ToolIsType(typeid(VisualToolScale))) return "video/tool/scale";
		if (c->videoDisplay->ToolIsType(typeid(VisualToolClip))) return "video/tool/clip";
		if (c->videoDisplay->ToolIsType(typeid(VisualToolVectorClip))) return "video/tool/vector_clip";
		if (c->videoDisplay->ToolIsType(typeid(VisualToolMaskEdit))) return "video/tool/mask_edit";
		if (c->videoDisplay->ToolIsType(typeid(VisualToolMask))) return "video/tool/mask";
		if (c->videoDisplay->ToolIsType(typeid(VisualToolShape))) return "video/tool/shape";
		return "video/tool/cross";
	}

	template<class T>
	struct visual_tool_command : public Command {
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

		bool Validate(const agi::Context *c) override {
			return !!c->project->VideoProvider();
		}

		bool IsActive(const agi::Context *c) override {
			return c->videoDisplay->ToolIsType(typeid(T));
		}

		void operator()(agi::Context *c) override {
			c->videoDisplay->SetTool(std::make_unique<T>(c->videoDisplay, c));
		}
	};

	template<VisualToolVectorClipMode M>
	struct visual_tool_vclip_command : public Command {
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

		bool Validate(const agi::Context *c) override {
			return !!c->project->VideoProvider();
		}

		bool IsActive(const agi::Context *c) override {
			return (c->videoDisplay->ToolIsType(typeid(VisualToolVectorClip)) ||
				c->videoDisplay->ToolIsType(typeid(VisualToolMaskEdit))) &&
				c->videoDisplay->GetSubTool() == M;
		}

		void operator()(agi::Context *c) override {
			if (!c->videoDisplay->ToolIsType(typeid(VisualToolVectorClip)) &&
				!c->videoDisplay->ToolIsType(typeid(VisualToolMaskEdit)))
				c->videoDisplay->SetTool(std::make_unique<VisualToolVectorClip>(c->videoDisplay, c));
			c->videoDisplay->SetSubTool(M);
		}
	};

	template<VisualToolMaskMode M>
	struct visual_tool_mask_command : public Command {
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

		bool Validate(const agi::Context *c) override {
			return !!c->project->VideoProvider();
		}

		bool IsActive(const agi::Context *c) override {
			return c->videoDisplay->ToolIsType(typeid(VisualToolMask)) && c->videoDisplay->GetSubTool() == M;
		}

		void operator()(agi::Context *c) override {
			if (!c->videoDisplay->ToolIsType(typeid(VisualToolMask)))
				c->videoDisplay->SetTool(std::make_unique<VisualToolMask>(c->videoDisplay, c));
			c->videoDisplay->SetSubTool(M);
		}
	};

	template<VisualToolPerspectiveSetting M>
	struct visual_tool_persp_setting : public Command {
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)

		bool Validate(const agi::Context *c) override {
			return c->videoDisplay->ToolIsType(typeid(VisualToolPerspective));
		}

		virtual const bool CheckActive(int subtool) {
			return subtool & M;
		}

		virtual const int UpdateSubTool(int subtool) {
			return subtool ^ M;
		}

		bool IsActive(const agi::Context *c) override {
			return Validate(c) && CheckActive(c->videoDisplay->GetSubTool());
		}

		void operator()(agi::Context *c) override {
			if (!c->videoDisplay->ToolIsType(typeid(VisualToolPerspective)))
				c->videoDisplay->SetTool(std::make_unique<VisualToolPerspective>(c->videoDisplay, c));
			c->videoDisplay->SetSubTool(UpdateSubTool(c->videoDisplay->GetSubTool()));
		}
	};

	struct visual_mode_cross final : public visual_tool_command<VisualToolCross> {
		CMD_NAME("video/tool/cross")
		CMD_ICON(visual_standard)
		STR_MENU("Standard")
		STR_DISP("Standard")
		STR_HELP("Standard mode, double click sets position")
	};

	struct visual_mode_textbox final : public Command {
		CMD_NAME("video/tool/textbox")
		wxBitmapBundle Icon(int height, wxLayoutDirection = wxLayout_LeftToRight) const override {
			return wxBitmapBundle::FromBitmap(MakeTextboxBitmap(height));
		}
		CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO | COMMAND_HIDE_INVALID)
		STR_MENU("Textbox")
		STR_DISP("Textbox")
		STR_HELP("Show and edit the selected textbox")

		bool Validate(const agi::Context *c) override {
			auto line = c->selectionController->GetActiveLine();
			return !!c->project->VideoProvider() && line && c->imageMask->IsTextBoxGroup(line);
		}

		bool IsActive(const agi::Context *c) override {
			return c->videoDisplay->ToolIsType(typeid(VisualToolTextBox));
		}

		void operator()(agi::Context *c) override {
			if (!Validate(c)) return;
			std::string return_tool = CurrentVisualTool(c);
			c->videoDisplay->SetTool(std::make_unique<VisualToolTextBox>(
				c->videoDisplay, c, false, std::move(return_tool)));
		}
	};

	struct visual_mode_drag final : public visual_tool_command<VisualToolDrag> {
		CMD_NAME("video/tool/drag")
		CMD_ICON(visual_move)
		STR_MENU("Drag")
		STR_DISP("Drag")
		STR_HELP("Drag subtitles")
	};

	struct visual_mode_rotate_z final : public visual_tool_command<VisualToolRotateZ> {
		CMD_NAME("video/tool/rotate/z")
		CMD_ICON(visual_rotatez)
		STR_MENU("Rotate Z")
		STR_DISP("Rotate Z")
		STR_HELP("Rotate subtitles on their Z axis")
	};

	struct visual_mode_rotate_xy final : public visual_tool_command<VisualToolRotateXY> {
		CMD_NAME("video/tool/rotate/xy")
		CMD_ICON(visual_rotatexy)
		STR_MENU("Rotate XY")
		STR_DISP("Rotate XY")
		STR_HELP("Rotate subtitles on their X and Y axes")
	};

	struct visual_mode_perspective final : public visual_tool_command<VisualToolPerspective> {
		CMD_NAME("video/tool/perspective")
		CMD_ICON(visual_perspective)
		STR_MENU("Apply 3D Perspective")
		STR_DISP("Apply 3D Perspective")
		STR_HELP("Rotate and shear subtitles to make them fit a given quad's perspective")
	};

	struct visual_mode_scale final : public visual_tool_command<VisualToolScale> {
		CMD_NAME("video/tool/scale")
		CMD_ICON(visual_scale)
		STR_MENU("Scale")
		STR_DISP("Scale")
		STR_HELP("Scale subtitles on X and Y axes")
	};

	struct visual_mode_clip final : public visual_tool_command<VisualToolClip> {
		CMD_NAME("video/tool/clip")
		CMD_ICON(visual_clip)
		STR_MENU("Clip")
		STR_DISP("Clip")
		STR_HELP("Clip subtitles to a rectangle")
	};

	struct visual_mode_vector_clip final : public visual_tool_command<VisualToolVectorClip> {
		CMD_NAME("video/tool/vector_clip")
		CMD_ICON(visual_vector_clip)
		STR_MENU("Vector Clip")
		STR_DISP("Vector Clip")
		STR_HELP("Clip subtitles to a vectorial area")
	};

	struct visual_mode_mask_edit final : public visual_tool_command<VisualToolMaskEdit> {
		CMD_NAME("video/tool/mask_edit")
		CMD_ICON(visual_mask_edit)
		STR_MENU("Mask Editing")
		STR_DISP("Mask Editing")
		STR_HELP("Edit the active line's ASS drawing")
	};

	struct visual_mode_mask final : public visual_tool_command<VisualToolMask> {
		CMD_NAME("video/tool/mask")
		CMD_ICON(visual_mask_create)
		STR_MENU("Masking")
		STR_DISP("Masking")
		STR_HELP("Create masks from areas drawn on the video")
	};

	struct visual_mode_shape final : public visual_tool_command<VisualToolShape> {
		CMD_NAME("video/tool/shape")
		CMD_ICON(visual_shape)
		STR_MENU("Add Shape")
		STR_DISP("Add Shape")
		STR_HELP("Draw a shape and add it as an ASS drawing")
	};

	// Perspective settings
	struct visual_mode_perspective_plane final : public visual_tool_persp_setting<PERSP_OUTER> {
		CMD_NAME("video/tool/perspective/plane")
		CMD_ICON(visual_perspective_plane)
		STR_MENU("Show Surrounding Plane")
		STR_DISP("Show Surrounding Plane")
		STR_HELP("Toggles showing a second quad for the ambient 3D plane.")
	};

	// Perspective settings
	struct visual_mode_perspective_lock_inner final : public visual_tool_persp_setting<PERSP_LOCK_OUTER> {
		CMD_NAME("video/tool/perspective/lock_outer")
		CMD_ICON(visual_perspective_lock_outer)
		STR_MENU("Lock Outer Quad")
		STR_DISP("Lock Outer Quad")
		STR_HELP("When the surrounding plane is also visible, switches which quad is locked. If inactive, the inner quad can only be resized without changing the perspective plane. If active, this holds for the outer quad instead.")

		bool Validate(const agi::Context *c) override {
			return c->videoDisplay->ToolIsType(typeid(VisualToolPerspective)) && c->videoDisplay->GetSubTool() & PERSP_OUTER;
		}
	};

	struct visual_mode_perspective_grid final : public visual_tool_persp_setting<PERSP_GRID> {
		CMD_NAME("video/tool/perspective/grid")
		CMD_ICON(visual_perspective_grid)
		STR_MENU("Show Grid")
		STR_DISP("Show Grid")
		STR_HELP("Toggles showing a 3D grid in the visual perspective tool")
	};

	struct visual_mode_perspective_orgmode_center : public visual_tool_persp_setting<PERSP_ORGMODE_CENTER> {
		CMD_NAME("video/tool/perspective/orgmode/center")
		CMD_ICON(visual_perspective_orgmode_center)
		STR_MENU("\\org Mode: Center")
		STR_DISP("\\org Mode: Center")
		STR_HELP("Puts \\org at the center of the perspective quad")

		const bool CheckActive(int subtool) override {
			return (subtool & PERSP_ORGMODE) == PERSP_ORGMODE_CENTER;
		}

		const int UpdateSubTool(int subtool) override {
			return (subtool & ~PERSP_ORGMODE) | PERSP_ORGMODE_CENTER;
		}
	};

	struct visual_mode_perspective_orgmode_nofax : public visual_tool_persp_setting<PERSP_ORGMODE_NOFAX> {
		CMD_NAME("video/tool/perspective/orgmode/nofax")
		CMD_ICON(visual_perspective_orgmode_nofax)
		STR_MENU("\\org Mode: No \\fax")
		STR_DISP("\\org Mode: No \\fax")
		STR_HELP("Finds a value for \\org where \\fax can be zero, if possible. Use this mode if your event contains line breaks.")

		const bool CheckActive(int subtool) override {
			return (subtool & PERSP_ORGMODE) == PERSP_ORGMODE_NOFAX;
		}

		const int UpdateSubTool(int subtool) override {
			return (subtool & ~PERSP_ORGMODE) | PERSP_ORGMODE_NOFAX;
		}
	};

	struct visual_mode_perspective_orgmode_keep : public visual_tool_persp_setting<PERSP_ORGMODE_KEEP> {
		CMD_NAME("video/tool/perspective/orgmode/keep")
		CMD_ICON(visual_perspective_orgmode_keep)
		STR_MENU("\\org Mode: Keep")
		STR_DISP("\\org Mode: Keep")
		STR_HELP("Fixes the position of \\org")

		const bool CheckActive(int subtool) override {
			return (subtool & PERSP_ORGMODE) == PERSP_ORGMODE_KEEP;
		}

		const int UpdateSubTool(int subtool) override {
			return (subtool & ~PERSP_ORGMODE) | PERSP_ORGMODE_KEEP;
		}
	};

	struct visual_mode_perspective_orgmode_cycle : public visual_tool_persp_setting<PERSP_ORGMODE> {
		CMD_NAME("video/tool/perspective/orgmode/cycle")
		STR_MENU("Cycle \\org mode")
		STR_DISP("Cycle \\org mode")
		STR_HELP("Cycles through the three \\org modes")

		const bool CheckActive(int subtool) override {
			return false;
		}

		const int UpdateSubTool(int subtool) override {
			int newtool = 0;
			switch (subtool & PERSP_ORGMODE) {
				case PERSP_ORGMODE_CENTER:
					newtool = PERSP_ORGMODE_NOFAX;
					break;
				case PERSP_ORGMODE_NOFAX:
					newtool = PERSP_ORGMODE_KEEP;
					break;
				case PERSP_ORGMODE_KEEP:
					newtool = PERSP_ORGMODE_CENTER;
					break;
				default:
					break;
			}
			return (subtool & ~PERSP_ORGMODE) | newtool;
		}
	};

	// Vector clip tools

	struct visual_mode_vclip_drag final : public visual_tool_vclip_command<VCLIP_DRAG> {
		CMD_NAME("video/tool/vclip/drag")
		CMD_ICON(visual_vector_clip_drag)
		STR_MENU("Drag")
		STR_DISP("Drag")
		STR_HELP("Drag control points")
	};

	struct visual_mode_vclip_line final : public visual_tool_vclip_command<VCLIP_LINE> {
		CMD_NAME("video/tool/vclip/line")
		CMD_ICON(visual_vector_clip_line)
		STR_MENU("Line")
		STR_DISP("Line")
		STR_HELP("Append a line")
	};
	struct visual_mode_vclip_bicubic final : public visual_tool_vclip_command<VCLIP_BICUBIC> {
		CMD_NAME("video/tool/vclip/bicubic")
		CMD_ICON(visual_vector_clip_bicubic)
		STR_MENU("Bicubic")
		STR_DISP("Bicubic")
		STR_HELP("Append a bezier bicubic curve")
	};
	template<VisualToolVectorClipUpdate Action>
	struct visual_mode_vclip_brush_action : public Command {
		CMD_TYPE(COMMAND_VALIDATE)

		bool Validate(const agi::Context *c) override {
			return !!c->project->VideoProvider();
		}

		void operator()(agi::Context *c) override {
			if (!c->videoDisplay->ToolIsType(typeid(VisualToolVectorClip)) &&
				!c->videoDisplay->ToolIsType(typeid(VisualToolMaskEdit)))
				c->videoDisplay->SetTool(std::make_unique<VisualToolVectorClip>(c->videoDisplay, c));
			c->videoDisplay->SetSubTool(VCLIP_BRUSH);
			c->videoDisplay->UpdateTool(Action);
		}
	};

	struct visual_mode_vclip_brush_add final : public visual_mode_vclip_brush_action<VCLIP_BRUSH_ACTION_ADD> {
		CMD_NAME("video/tool/vclip/brush_add")
		wxBitmapBundle Icon(int height, wxLayoutDirection = wxLayout_LeftToRight) const override {
			return wxBitmapBundle::FromBitmap(MakeVisualVectorClipBrushBitmap(true, height, false));
		}
		STR_MENU("Brush add")
		STR_DISP("Brush add")
		STR_HELP("Add to the current vector clip with a brush")
	};

	struct visual_mode_vclip_brush_delete final : public visual_mode_vclip_brush_action<VCLIP_BRUSH_ACTION_DELETE> {
		CMD_NAME("video/tool/vclip/brush_delete")
		wxBitmapBundle Icon(int height, wxLayoutDirection = wxLayout_LeftToRight) const override {
			return wxBitmapBundle::FromBitmap(MakeVisualVectorClipBrushBitmap(false, height, false));
		}
		STR_MENU("Brush delete")
		STR_DISP("Brush delete")
		STR_HELP("Delete from the current vector clip with a brush")
	};
	struct visual_mode_vclip_convert final : public visual_tool_vclip_command<VCLIP_CONVERT> {
		CMD_NAME("video/tool/vclip/convert")
		CMD_ICON(visual_vector_clip_convert)
		STR_MENU("Convert")
		STR_DISP("Convert")
		STR_HELP("Convert a segment between line and bicubic")
	};
	struct visual_mode_vclip_insert final : public visual_tool_vclip_command<VCLIP_INSERT> {
		CMD_NAME("video/tool/vclip/insert")
		CMD_ICON(visual_vector_clip_insert)
		STR_MENU("Insert")
		STR_DISP("Insert")
		STR_HELP("Insert a control point")
	};
	struct visual_mode_vclip_append final : public visual_tool_vclip_command<VCLIP_APPEND> {
		CMD_NAME("video/tool/vclip/append")
		CMD_ICON(visual_vector_clip_append)
		STR_MENU("Append")
		STR_DISP("Append")
		STR_HELP("Append a control point")
	};
	struct visual_mode_vclip_remove final : public visual_tool_vclip_command<VCLIP_REMOVE> {
		CMD_NAME("video/tool/vclip/remove")
		CMD_ICON(visual_vector_clip_remove)
		STR_MENU("Remove")
		STR_DISP("Remove")
		STR_HELP("Remove a control point")
	};
	struct visual_mode_vclip_freehand final : public visual_tool_vclip_command<VCLIP_FREEHAND> {
		CMD_NAME("video/tool/vclip/freehand")
		CMD_ICON(visual_vector_clip_freehand)
		STR_MENU("Freehand")
		STR_DISP("Freehand")
		STR_HELP("Draw a freehand shape")
	};
	struct visual_mode_vclip_freehand_smooth final : public visual_tool_vclip_command<VCLIP_FREEHAND_SMOOTH> {
		CMD_NAME("video/tool/vclip/freehand_smooth")
		CMD_ICON(visual_vector_clip_freehand_smooth)
		STR_MENU("Freehand smooth")
		STR_DISP("Freehand smooth")
		STR_HELP("Draw a smoothed freehand shape")
	};

	struct visual_mode_vclip_color final : public visual_tool_vclip_command<VCLIP_COLOR> {
		CMD_NAME("video/tool/vclip/color")
		wxBitmapBundle Icon(int height, wxLayoutDirection = wxLayout_LeftToRight) const override {
			return GETBUNDLE(eyedropper_tool, height);
		}
		STR_MENU("Extract by color")
		STR_DISP("Extract by color")
		STR_HELP("Add contours from similar colors in a selected range")
	};

	struct visual_mode_mask_rectangle final : public visual_tool_mask_command<MASK_RECTANGLE> {
		CMD_NAME("video/tool/mask/rectangle")
		CMD_ICON(visual_clip)
		STR_MENU("Rectangle")
		STR_DISP("Rectangle")
		STR_HELP("Draw a rectangular mask")
	};

	struct visual_mode_mask_points final : public visual_tool_mask_command<MASK_POINTS> {
		CMD_NAME("video/tool/mask/points")
		CMD_ICON(visual_vector_clip_line)
		STR_MENU("Points")
		STR_DISP("Points")
		STR_HELP("Add mask points")
	};

	struct visual_mode_mask_brush final : public visual_tool_mask_command<MASK_BRUSH> {
		CMD_NAME("video/tool/mask/brush")
		wxBitmapBundle Icon(int height, wxLayoutDirection = wxLayout_LeftToRight) const override {
			return wxBitmapBundle::FromBitmap(MakeVisualVectorClipBrushBitmap(true, height, false));
		}
		STR_MENU("Brush")
		STR_DISP("Brush")
		STR_HELP("Paint a mask with a brush")
	};

	struct visual_mode_mask_freehand final : public visual_tool_mask_command<MASK_FREEHAND> {
		CMD_NAME("video/tool/mask/freehand")
		CMD_ICON(visual_vector_clip_freehand)
		STR_MENU("Freehand")
		STR_DISP("Freehand")
		STR_HELP("Draw a freehand mask")
	};

	struct visual_mode_mask_color final : public visual_tool_mask_command<MASK_COLOR> {
		CMD_NAME("video/tool/mask/color")
		wxBitmapBundle Icon(int height, wxLayoutDirection = wxLayout_LeftToRight) const override {
			return GETBUNDLE(eyedropper_tool, height);
		}
		STR_MENU("Extract by color")
		STR_DISP("Extract by color")
		STR_HELP("Create mask contours from similar colors in a selected range")
	};

	struct visual_mode_drag_change final : public Command {
		CMD_NAME("video/tool/drag/change")
		CMD_TYPE(COMMAND_DYNAMIC_NAME | COMMAND_DYNAMIC_HELP | COMMAND_DYNAMIC_ICON)

		bool Validate(const agi::Context *c) override {
			return !!c->project->VideoProvider();
		}

		wxBitmapBundle Icon(int height, wxLayoutDirection dir = wxLayout_LeftToRight) const override {
			int mode = OPT_GET("Tool/Drag Type")->GetInt();

			if (mode == 1)
				return GETBUNDLEDIR(drag_lockx, height, dir);
			
			if (mode == 2)
				return GETBUNDLEDIR(drag_locky, height, dir);

			return GETBUNDLEDIR(drag_nolock, height, dir);
		}

		wxString StrMenu(const agi::Context *c) const override {
			int mode = OPT_GET("Tool/Drag Type")->GetInt();

			if (mode == 1)
				return _("X locked");
			
			if (mode == 2)
				return _("Y locked");

			return _("Toggle locking");
		}

		wxString StrDisplay(const agi::Context *c) const override {
			return StrMenu(nullptr);
		}

		wxString StrHelp() const override {
			return StrMenu(nullptr);
		}

		void operator()(agi::Context *c) override {
			int mode = OPT_GET("Tool/Drag Type")->GetInt();
			mode = mode + 1;

			if (mode > 2)
				mode = 0;

			OPT_SET("Tool/Drag Type")->SetInt(mode);

			if (c->videoDisplay->ToolIsType(typeid(VisualToolDrag))) {
				c->videoDisplay->UpdateTool(DRAG_LOCK);
			}
		}
	};
}

namespace cmd {
	void init_visual_tools() {
		reg(std::make_unique<visual_mode_textbox>());
		reg(std::make_unique<visual_mode_cross>());
		reg(std::make_unique<visual_mode_drag>());
		reg(std::make_unique<visual_mode_rotate_z>());
		reg(std::make_unique<visual_mode_rotate_xy>());
		reg(std::make_unique<visual_mode_perspective>());
		reg(std::make_unique<visual_mode_scale>());
		reg(std::make_unique<visual_mode_clip>());
		reg(std::make_unique<visual_mode_vector_clip>());
		reg(std::make_unique<visual_mode_mask_edit>());
		reg(std::make_unique<visual_mode_mask>());
		reg(std::make_unique<visual_mode_shape>());

		reg(std::make_unique<visual_mode_vclip_drag>());
		reg(std::make_unique<visual_mode_vclip_line>());
		reg(std::make_unique<visual_mode_vclip_bicubic>());
		reg(std::make_unique<visual_mode_vclip_brush_add>());
		reg(std::make_unique<visual_mode_vclip_brush_delete>());
		reg(std::make_unique<visual_mode_vclip_convert>());
		reg(std::make_unique<visual_mode_vclip_insert>());
		reg(std::make_unique<visual_mode_vclip_append>());
		reg(std::make_unique<visual_mode_vclip_remove>());
		reg(std::make_unique<visual_mode_vclip_freehand>());
		reg(std::make_unique<visual_mode_vclip_freehand_smooth>());
		reg(std::make_unique<visual_mode_vclip_color>());
		reg(std::make_unique<visual_mode_mask_rectangle>());
		reg(std::make_unique<visual_mode_mask_points>());
		reg(std::make_unique<visual_mode_mask_brush>());
		reg(std::make_unique<visual_mode_mask_freehand>());
		reg(std::make_unique<visual_mode_mask_color>());

		reg(std::make_unique<visual_mode_perspective_plane>());
		reg(std::make_unique<visual_mode_perspective_lock_inner>());
		reg(std::make_unique<visual_mode_perspective_grid>());
		reg(std::make_unique<visual_mode_perspective_orgmode_center>());
		reg(std::make_unique<visual_mode_perspective_orgmode_nofax>());
		reg(std::make_unique<visual_mode_perspective_orgmode_keep>());
		reg(std::make_unique<visual_mode_perspective_orgmode_cycle>());

		reg(std::make_unique<visual_mode_drag_change>());
	}
}
