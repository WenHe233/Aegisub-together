// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include <string>
#include <vector>

namespace typesetting::motion::detail {

std::string Number(double value);

void SetFirstTag(std::string& text, std::string const& name, std::string value,
	std::vector<std::string> aliases = {});

void RemoveFirstTag(std::string& text, std::string const& name,
	std::vector<std::string> aliases = {});

/// Write a generated numeric override only if it differs from the effective
/// style/default value after ASS number formatting.
void SetFirstTagUnlessDefault(std::string& text, std::string const& name,
	double value, double default_value, std::vector<std::string> aliases = {});

} // namespace typesetting::motion::detail
