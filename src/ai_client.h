// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

#include <libaegisub/fs.h>

namespace ai {

class Error final : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

struct SubtitleLine {
	int id = 0;
	int start_ms = 0;
	int end_ms = 0;
	std::string source_text;
	std::string current_text;
	std::string actor;
	std::string style;
	std::string ass_text;
	bool target = false;
};

struct ProofreadIssue {
	int line_id = 0;
	std::vector<std::string> categories;
	std::string explanation;
	std::vector<std::string> suggestions;
};

struct ProofreadResult {
	std::string message;
	std::vector<ProofreadIssue> issues;
};

struct LineReview {
	int id = 0;
	std::string japanese;
	std::string romaji;
	std::string verdict;
	std::string assessment;
	std::vector<std::string> issues;
	std::string suggested_text;
};

struct ReviewResult {
	std::string message;
	std::vector<LineReview> lines;
	/// Serialized Responses API input items. Kept only for the lifetime of the
	/// modal conversation and resent with store=false for follow-up turns.
	std::string conversation_json = "[]";
};

class OpenAIClient final {
	std::string api_key;
	std::string model;
	std::string transcription_model;
	std::string custom_instructions;
	std::atomic_bool *cancelled = nullptr;

public:
	OpenAIClient(std::string api_key, std::string model,
		std::string transcription_model, std::string custom_instructions = {},
		std::atomic_bool *cancelled = nullptr);

	void TestConnection() const;
	std::string Transcribe(agi::fs::path const& audio_file) const;
	ReviewResult Review(std::vector<SubtitleLine> const& lines,
		std::string const& japanese_transcript) const;
	ReviewResult Continue(ReviewResult const& previous,
		std::string const& user_message) const;
	ProofreadResult Proofread(std::vector<SubtitleLine> const& lines) const;
};

enum class ApiKeySource {
	None,
	Session,
	Environment,
	CredentialManager
};

/// The session key takes precedence, followed by OPENAI_API_KEY and the OS
/// credential store. The returned value must never be logged.
std::string GetApiKey();
ApiKeySource GetApiKeySource();
void SetSessionApiKey(std::string key);
void ClearSessionApiKey();
bool StoreApiKey(std::string const& key, std::string *error = nullptr);
bool DeleteStoredApiKey(std::string *error = nullptr);
bool HasStoredApiKey();

} // namespace ai
