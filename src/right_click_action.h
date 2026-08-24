// Copyright (c) 2026, Aegisub contributors
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

#pragma once

#include <functional>
#include <utility>
#include <wx/window.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

namespace right_click_action {

#ifdef __WXGTK__
inline void OnPressed(GtkGestureMultiPress *, gint press_count, gdouble, gdouble,
	gpointer data) {
	if (press_count == 1)
		(*static_cast<std::function<void()> *>(data))();
}

inline void DeleteAction(gpointer data, GClosure *) {
	delete static_cast<std::function<void()> *>(data);
}

inline void DeleteGesture(gpointer data) {
	g_object_unref(data);
}
#endif

/// Bind an action to a secondary mouse-button press on a native control.
/// wxGTK's range controls do not consistently turn an unfocused secondary
/// click into a wxMouseEvent, so listen to the underlying GTK widget there.
inline void Bind(wxWindow *window, std::function<void()> action) {
#ifdef __WXGTK__
	auto widget = GTK_WIDGET(window->GetHandle());
	auto gesture = gtk_gesture_multi_press_new(widget);
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
		GDK_BUTTON_SECONDARY);
	// Capture the press before the native range/combobox focus and drag
	// handlers can claim its event sequence.
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture),
		GTK_PHASE_CAPTURE);
	g_signal_connect_data(gesture, "pressed", G_CALLBACK(OnPressed),
		new std::function<void()>(std::move(action)), DeleteAction,
		static_cast<GConnectFlags>(0));
	// gtk_gesture_multi_press_new transfers ownership to the caller. Keep that
	// reference for exactly as long as the native control exists.
	g_object_set_data_full(G_OBJECT(widget), "aegisub-right-click-action",
		gesture, DeleteGesture);
#else
	window->Bind(wxEVT_RIGHT_DOWN,
		[action = std::move(action)](wxMouseEvent &) { action(); });
#endif
}

}
