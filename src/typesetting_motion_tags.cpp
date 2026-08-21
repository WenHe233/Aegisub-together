// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_motion_tags.h"

#include <libaegisub/format.h>

#include <cmath>
#include <regex>
#include <utility>

namespace typesetting::motion::detail {
namespace {

void RemoveFromBlock(std::string& block, std::string const& tag) {
	std::string plain = !tag.empty() && tag[0] == '\\' ? tag.substr(1) : tag;
	std::regex pattern("\\\\" + plain +
		R"((?:\([^)]*\)|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?))");
	block = std::regex_replace(block, pattern, "");
}

} // namespace

std::string Number(double value) {
	std::string out = agi::format("%.3f", value);
	while (out.size() > 1 && out.back() == '0') out.pop_back();
	if (!out.empty() && out.back() == '.') out.pop_back();
	return out == "-0" ? "0" : out;
}

void SetFirstTag(std::string& text, std::string const& name, std::string value,
	std::vector<std::string> aliases) {
	if (text.empty() || text[0] != '{') text = "{}" + text;
	size_t close = text.find('}');
	if (close == std::string::npos) { text = "{}" + text; close = 1; }
	std::string block = text.substr(0, close + 1);
	RemoveFromBlock(block, name);
	for (auto const& alias : aliases) RemoveFromBlock(block, alias);
	block.insert(block.size() - 1, name + std::move(value));
	text.replace(0, close + 1, block);
}

void RemoveFirstTag(std::string& text, std::string const& name,
	std::vector<std::string> aliases) {
	if (text.empty() || text[0] != '{') return;
	size_t close = text.find('}');
	if (close == std::string::npos) return;
	std::string block = text.substr(0, close + 1);
	RemoveFromBlock(block, name);
	for (auto const& alias : aliases) RemoveFromBlock(block, alias);
	text.replace(0, close + 1, block);
}

void SetFirstTagUnlessDefault(std::string& text, std::string const& name,
	double value, double default_value, std::vector<std::string> aliases) {
	// Number() writes three decimal places at most. Treat values which would render
	// identically as equal so solver noise cannot create a useless override tag.
	if (std::abs(value - default_value) < .0005)
		RemoveFirstTag(text, name, std::move(aliases));
	else
		SetFirstTag(text, name, Number(value), std::move(aliases));
}

} // namespace typesetting::motion::detail
