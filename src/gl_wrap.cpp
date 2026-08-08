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

/// @file gl_wrap.cpp
/// @brief Convenience functions for drawing various geometric primitives on an OpenGL surface
/// @ingroup video_output
///

#include "gl_wrap.h"

#include <wx/colour.h>
#include <wx/image.h>

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#ifdef HAVE_GL_GLEXT_H
#include <GL/glext.h>
#else
#include "gl/glext.h"
#endif
#endif

static const float deg2rad = 3.1415926536f / 180.f;
static const float pi = 3.1415926535897932384626433832795f;

void OpenGLWrapper::DrawImage(wxImage const& image, Vector2D top_left, Vector2D bottom_right) const {
	if (!image.IsOk() || image.GetWidth() <= 0 || image.GetHeight() <= 0) return;
	auto rgb = image.GetData();
	auto alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
	bool refresh = !image_texture || image_data_key != rgb || image_alpha_key != alpha ||
		image_texture_width != image.GetWidth() || image_texture_height != image.GetHeight();
	if (refresh) {
		size_t pixels = static_cast<size_t>(image.GetWidth()) * image.GetHeight();
		std::vector<unsigned char> rgba(pixels * 4);
		for (size_t i = 0; i < pixels; ++i) {
			rgba[i * 4] = rgb[i * 3];
			rgba[i * 4 + 1] = rgb[i * 3 + 1];
			rgba[i * 4 + 2] = rgb[i * 3 + 2];
			rgba[i * 4 + 3] = alpha ? alpha[i] : 255;
		}
		if (!image_texture) glGenTextures(1, &image_texture);
		glBindTexture(GL_TEXTURE_2D, image_texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.GetWidth(), image.GetHeight(), 0,
			GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		image_data_key = rgb;
		image_alpha_key = alpha;
		image_texture_width = image.GetWidth();
		image_texture_height = image.GetHeight();
	}
	glBindTexture(GL_TEXTURE_2D, image_texture);

	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1.f, 1.f, 1.f, 1.f);
	glBegin(GL_QUADS);
	glTexCoord2f(0.f, 0.f); glVertex2f(top_left.X(), top_left.Y());
	glTexCoord2f(1.f, 0.f); glVertex2f(bottom_right.X(), top_left.Y());
	glTexCoord2f(1.f, 1.f); glVertex2f(bottom_right.X(), bottom_right.Y());
	glTexCoord2f(0.f, 1.f); glVertex2f(top_left.X(), bottom_right.Y());
	glEnd();
	glDisable(GL_TEXTURE_2D);
}

#ifdef __WIN32__
#define glGetProc(a) wglGetProcAddress(a)
#elif !defined(__APPLE__)
#include <GL/glx.h>
#define glGetProc(a) glXGetProcAddress((const GLubyte *)(a))
#endif

#if defined(__APPLE__)
// Not required on OS X.
#define APIENTRY
#define GL_EXT(type, name)
#else
#define GL_EXT(type, name) \
	static type name = reinterpret_cast<type>(glGetProc(#name)); \
	if (!name) { \
	name = reinterpret_cast<type>(& name ## Fallback); \
	}
#endif

class VertexArray {
	std::vector<float> data;
	size_t dim;
public:
	VertexArray(size_t dims, size_t elems) {
		SetSize(dims, elems);
	}

	void SetSize(size_t dims, size_t elems) {
		dim = dims;
		data.resize(elems * dim);
	}

	void Set(size_t i, float x, float y) {
		data[i * dim] = x;
		data[i * dim + 1] = y;
	}

	void Set(size_t i, float x, float y, float z) {
		data[i * dim] = x;
		data[i * dim + 1] = y;
		data[i * dim + 2] = z;
	}

	void Set(size_t i, Vector2D p) {
		data[i * dim] = p.X();
		data[i * dim + 1] = p.Y();
	}

	void Draw(GLenum mode, bool clear = true) {
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(dim, GL_FLOAT, 0, &data[0]);
		glDrawArrays(mode, 0, data.size() / dim);
		glDisableClientState(GL_VERTEX_ARRAY);
		if (clear)
			data.clear();
	}
};

OpenGLWrapper::OpenGLWrapper() {
	line_r = line_g = line_b = line_a = 1.f;
	fill_r = fill_g = fill_b = fill_a = 1.f;
	line_width = 1;
	transform_pushed = false;
	smooth = true;
}

void OpenGLWrapper::DrawLine(Vector2D p1, Vector2D p2) const {
	SetModeLine();
	VertexArray buf(2, 2);
	buf.Set(0, p1);
	buf.Set(1, p2);
	buf.Draw(GL_LINES);
}

static inline Vector2D interp(Vector2D p1, Vector2D p2, float t) {
	return t * p1 + (1 - t) * p2;
}

void OpenGLWrapper::DrawDashedLine(Vector2D p1, Vector2D p2, float step) const {
	step /= (p2 - p1).Len();
	for (float t = 0; t < 1.f; t += 2 * step) {
		DrawLine(interp(p1, p2, t), interp(p1, p2, t + step));
	}
}

void OpenGLWrapper::DrawEllipse(Vector2D center, Vector2D radius) const {
	DrawRing(center, radius.Y(), radius.Y(), radius.X() / radius.Y());
}

void OpenGLWrapper::DrawRectangle(Vector2D p1, Vector2D p2) const {
	VertexArray buf(2, 4);
	buf.Set(0, p1);
	buf.Set(1, Vector2D(p2, p1));
	buf.Set(2, p2);
	buf.Set(3, Vector2D(p1, p2));

	// Fill
	if (fill_a != 0.f) {
		SetModeFill();
		buf.Draw(GL_QUADS, false);
	}
	// Outline
	if (line_a != 0.f) {
		SetModeLine();
		buf.Draw(GL_LINE_LOOP);
	}
}

void OpenGLWrapper::DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) const {
	VertexArray buf(2, 3);
	buf.Set(0, p1);
	buf.Set(1, p2);
	buf.Set(2, p3);

	// Fill
	if (fill_a != 0.f) {
		SetModeFill();
		buf.Draw(GL_TRIANGLES, false);
	}
	// Outline
	if (line_a != 0.f) {
		SetModeLine();
		buf.Draw(GL_LINE_LOOP);
	}
}

void OpenGLWrapper::DrawRing(Vector2D center, float r1, float r2, float ar, float arc_start, float arc_end) const {
	if (r2 > r1)
		std::swap(r1, r2);

	// Arc range
	bool needs_end_caps = arc_start != arc_end;

	arc_end *= deg2rad;
	arc_start *= deg2rad;
	if (arc_end <= arc_start)
		arc_end += 2.f * pi;
	float range = arc_end - arc_start;

	// Math
	int steps = std::max<int>(((r1 + r1 * ar) * range / (2.f * pi)) * 4, 12);
	float step = range / steps;
	float cur_angle = arc_start;

	VertexArray buf(2, steps);

	Vector2D scale_inner = Vector2D(ar, 1) * r1;
	Vector2D scale_outer = Vector2D(ar, 1) * r2;

	if (fill_a != 0.f) {
		SetModeFill();

		// Annulus
		if (r1 != r2) {
			buf.SetSize(2, (steps + 1) * 2);
			for (int i = 0; i <= steps; i++) {
				Vector2D offset = Vector2D::FromAngle(cur_angle);
				buf.Set(i * 2 + 0, center + offset * scale_inner);
				buf.Set(i * 2 + 1, center + offset * scale_outer);
				cur_angle += step;
			}
			buf.Draw(GL_QUAD_STRIP);
		}
		// Circle
		else {
			buf.SetSize(2, steps);
			for (int i = 0; i < steps; i++) {
				buf.Set(i, center + Vector2D::FromAngle(cur_angle) * scale_inner);
				cur_angle += step;
			}
			buf.Draw(GL_POLYGON);
		}

		cur_angle = arc_start;
	}

	if (line_a == 0.f) return;

	// Outer
	steps++;
	buf.SetSize(2, steps);

	SetModeLine();
	for (int i = 0; i < steps; i++) {
		buf.Set(i, center + Vector2D::FromAngle(cur_angle) * scale_outer);
		cur_angle += step;
	}
	buf.Draw(GL_LINE_STRIP);

	// Inner
	if (r1 == r2) return;

	cur_angle = arc_start;
	buf.SetSize(2, steps);
	for (int i = 0; i < steps; i++) {
		buf.Set(i, center + Vector2D::FromAngle(cur_angle) * scale_inner);
		cur_angle += step;
	}
	buf.Draw(GL_LINE_STRIP);

	if (!needs_end_caps) return;

	buf.SetSize(2, 4);
	buf.Set(0, center + Vector2D::FromAngle(arc_start) * scale_inner);
	buf.Set(1, center + Vector2D::FromAngle(arc_start) * scale_outer);
	buf.Set(2, center + Vector2D::FromAngle(arc_end) * scale_inner);
	buf.Set(3, center + Vector2D::FromAngle(arc_end) * scale_outer);
	buf.Draw(GL_LINES);
}

void OpenGLWrapper::SetLineColour(wxColour col, float alpha, int width) {
	line_r = col.Red() / 255.f;
	line_g = col.Green() / 255.f;
	line_b = col.Blue() / 255.f;
	line_a = alpha;
	line_width = width;
}

void OpenGLWrapper::SetFillColour(wxColour col, float alpha) {
	fill_r = col.Red() / 255.f;
	fill_g = col.Green() / 255.f;
	fill_b = col.Blue() / 255.f;
	fill_a = alpha;
}

void OpenGLWrapper::SetModeLine() const {
	glColor4f(line_r, line_g, line_b, line_a);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glLineWidth(line_width);
	if (smooth)
		glEnable(GL_LINE_SMOOTH);
	else
		glDisable(GL_LINE_SMOOTH);
}

void OpenGLWrapper::SetModeFill() const {
	glColor4f(fill_r, fill_g, fill_b, fill_a);
	if (fill_a == 1.f) glDisable(GL_BLEND);
	else {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
}

void OpenGLWrapper::SetInvert() {
	glEnable(GL_COLOR_LOGIC_OP);
	glLogicOp(GL_INVERT);

	// GL_LINE_SMOOTH combines badly with inverting
	smooth = false;
}

void OpenGLWrapper::ClearInvert() {
	glDisable(GL_COLOR_LOGIC_OP);
	smooth = true;
}

bool OpenGLWrapper::IsExtensionSupported(const char *ext) {
	char *extList = (char * )glGetString(GL_EXTENSIONS);
	return extList && !!strstr(extList, ext);
}

void OpenGLWrapper::DrawLines(size_t dim, std::vector<float> const& lines) {
	DrawLines(dim, &lines[0], lines.size() / dim);
}

void OpenGLWrapper::DrawLines(size_t dim, std::vector<float> const& lines, size_t c_dim, std::vector<float> const& colors) {
	glShadeModel(GL_SMOOTH);
	glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(c_dim, GL_FLOAT, 0, &colors[0]);
	DrawLines(dim, &lines[0], lines.size() / dim);
	glDisableClientState(GL_COLOR_ARRAY);
	glShadeModel(GL_FLAT);
}

void OpenGLWrapper::DrawLines(size_t dim, const float *lines, size_t n) {
	SetModeLine();
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(dim, GL_FLOAT, 0, lines);
	glDrawArrays(GL_LINES, 0, n);
	glDisableClientState(GL_VERTEX_ARRAY);
}

void OpenGLWrapper::DrawLineStrip(size_t dim, std::vector<float> const& lines) {
	if (lines.empty()) return;
	DrawLineStrip(dim, lines.data(), lines.size() / dim);
}

OpenGLWrapper::~OpenGLWrapper() {
	if (image_texture) glDeleteTextures(1, &image_texture);
}

void OpenGLWrapper::DrawLineStrip(size_t dim, const float *lines, size_t n) {
	if (!lines || !n) return;
	SetModeLine();
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(dim, GL_FLOAT, 0, lines);
	glDrawArrays(GL_LINE_STRIP, 0, n);
	glDisableClientState(GL_VERTEX_ARRAY);
}

// Substitute for glMultiDrawArrays for sub-1.4 OpenGL
// Not required on OS X.
#ifndef __APPLE__
static void APIENTRY glMultiDrawArraysFallback(GLenum mode, const GLint *first, const GLsizei *count, GLsizei primcount) {
	for (int i = 0; i < primcount; ++i) {
		glDrawArrays(mode, *first++, *count++);
	}
}
#endif

void OpenGLWrapper::DrawMultiPolygon(std::vector<float> const& points, std::vector<int> &start, std::vector<int> &count, Vector2D video_pos, Vector2D video_size, bool invert) {
	// Broken or sub-pixel duplicate vertices can make legacy OpenGL discard an
	// entire line loop. Sanitize each contour and collapse details which cannot
	// affect a pixel before submitting the multi-draw call.
	std::vector<float> clean_points;
	std::vector<int> clean_start;
	std::vector<int> clean_count;
	size_t contour_count = std::min(start.size(), count.size());
	for (size_t contour = 0; contour < contour_count; ++contour) {
		int first = std::max(0, start[contour]);
		int available = static_cast<int>(points.size() / 2) - first;
		int length = std::min(std::max(0, count[contour]), std::max(0, available));
		std::vector<Vector2D> clean;
		clean.reserve(length);
		for (int i = 0; i < length; ++i) {
			Vector2D point(points[(first + i) * 2], points[(first + i) * 2 + 1]);
			if (!std::isfinite(point.X()) || !std::isfinite(point.Y())) continue;
			if (!clean.empty() && (point - clean.back()).SquareLen() < .0625f) continue;
			clean.push_back(point);
		}
		if (clean.size() > 2 && (clean.front() - clean.back()).SquareLen() < .0625f)
			clean.pop_back();
		if (clean.size() < 3) continue;
		clean_start.push_back(static_cast<int>(clean_points.size() / 2));
		clean_count.push_back(static_cast<int>(clean.size()));
		for (auto point : clean) {
			clean_points.push_back(point.X());
			clean_points.push_back(point.Y());
		}
	}
	if (clean_points.empty()) return;

	GL_EXT(PFNGLMULTIDRAWARRAYSPROC, glMultiDrawArrays);

	float real_line_a = line_a;
	line_a = 0;

	// The following is nonzero winding-number PIP based on stencils

	// Draw to stencil only
	glEnable(GL_STENCIL_TEST);
	glColorMask(0, 0, 0, 0);

	// GL_INCR_WRAP was added in 1.4, so instead set the entire stencil to 128
	// and wobble from there
	glStencilFunc(GL_NEVER, 128, 0xFF);
	glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

	Vector2D video_max = video_pos + video_size;
	DrawRectangle(video_pos, video_max);

	// Increment the winding number for each forward facing triangle
	glStencilOp(GL_INCR, GL_INCR, GL_INCR);
	glEnable(GL_CULL_FACE);

	glCullFace(GL_BACK);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, clean_points.data());
	glMultiDrawArrays(GL_TRIANGLE_FAN, clean_start.data(), clean_count.data(), clean_start.size());

	// Decrement the winding number for each backfacing triangle
	glStencilOp(GL_DECR, GL_DECR, GL_DECR);
	glCullFace(GL_FRONT);
	glMultiDrawArrays(GL_TRIANGLE_FAN, clean_start.data(), clean_count.data(), clean_start.size());
	glDisable(GL_CULL_FACE);

	// Draw the actual rectangle
	glColorMask(1, 1, 1, 1);

	// VSFilter draws when the winding number is nonzero, so we want to draw the
	// mask when the winding number is zero (where 128 is zero due to the lack of
	// wrapping combined with unsigned numbers)
	glStencilFunc(invert ? GL_EQUAL : GL_NOTEQUAL, 128, 0xFF);
	DrawRectangle(video_pos, video_max);
	glDisable(GL_STENCIL_TEST);

	// Draw lines
	line_a = real_line_a;
	SetModeLine();
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, clean_points.data());
	glMultiDrawArrays(GL_LINE_LOOP, clean_start.data(), clean_count.data(), clean_start.size());

	glDisableClientState(GL_VERTEX_ARRAY);
}

void OpenGLWrapper::SetOrigin(Vector2D origin) {
	PrepareTransform();
	glTranslatef(origin.X(), origin.Y(), -1.f);
}

void OpenGLWrapper::SetScale(Vector2D scale) {
	PrepareTransform();
	glScalef(scale.X() / 100.f, scale.Y() / 100.f, 1.f);
}

void OpenGLWrapper::SetRotation(float x, float y, float z, float zScale) {
	PrepareTransform();
	float matrix[16] = { 2500, 0, 0, 0, 0, 2500, 0, 0, 0, 0, 1, 1, 0, 0, 2500, 2500 };
	glMultMatrixf(matrix);
	glScalef(1.f, 1.f, 8.f / zScale);
	glRotatef(y, 0.f, -1.f, 0.f);
	glRotatef(x, -1.f, 0.f, 0.f);
	glRotatef(z, 0.f, 0.f, -1.f);
}

void OpenGLWrapper::SetShear(float x, float y) {
	PrepareTransform();
	float matrix[16] = {
		1, y, 0, 0,
		x, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1
	};
	glMultMatrixf(matrix);
}

void OpenGLWrapper::PrepareTransform() {
	if (!transform_pushed) {
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();
		transform_pushed = true;
	}
}

void OpenGLWrapper::ResetTransform() {
	if (transform_pushed) {
		glPopMatrix();
		transform_pushed = false;
	}
}
