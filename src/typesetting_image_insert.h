// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

class wxImage;
namespace agi { struct Context; }

namespace typesetting::image_insert {

void Insert(agi::Context *context);
void EditWithImageEditor(agi::Context *context);
void InsertEditedImage(agi::Context *context, wxImage const& image);
void QuickInsert(agi::Context *context);
void ShowSettings(agi::Context *context);

} // namespace typesetting::image_insert
