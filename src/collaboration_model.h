// Copyright (c) 2026, Aegisub Together contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <libaegisub/collaboration.h>

#include <cstdint>
#include <string>
#include <unordered_map>

class AssFile;

namespace agi { namespace collab { namespace ass {

std::string GetMetadataValue(AssFile const& file, std::vector<std::uint32_t> const& ids, std::string const& key);
SanitizeResult SanitizeFileMetadata(AssFile& file, IdAllocator& allocator, SanitizeContext const& context);
Snapshot CaptureSnapshot(AssFile const& file, std::unordered_map<std::string, std::int64_t> const& line_versions,
	std::int64_t styles_version = 1, std::int64_t script_info_version = 1);
void LoadSnapshot(AssFile& file, Snapshot const& snapshot);

} } }
