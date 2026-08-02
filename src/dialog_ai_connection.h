// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

class wxWindow;

/// Configure the per-user OpenAI connection. Returns true when the dialog was
/// accepted and an API key is available (or when require_key is false).
bool ShowAIConnectionDialog(wxWindow *parent, bool require_key = false);
