// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "ai_client.h"

#include "format.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#endif

namespace ai {
namespace {

constexpr char api_base[] = "https://api.openai.com/v1";
constexpr size_t proofread_max_input_chars = 900000;
constexpr size_t proofread_max_lines_per_request = 300;
#ifdef _WIN32
constexpr wchar_t credential_target[] = L"MutekiAegisub/AI/OpenAI/default";
#endif

std::mutex session_key_mutex;
std::string session_key;

size_t append_response(char *contents, size_t size, size_t nmemb, void *target) {
	static_cast<std::string *>(target)->append(contents, size * nmemb);
	return size * nmemb;
}

int progress_callback(void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto cancelled = static_cast<std::atomic_bool *>(data);
	return cancelled && cancelled->load() ? 1 : 0;
}

class CurlHandle final {
	CURL *handle = curl_easy_init();

public:
	CurlHandle(CurlHandle const&) = delete;
	CurlHandle& operator=(CurlHandle const&) = delete;
	CurlHandle() {
		if (!handle)
			throw Error("A libcurl inicializálása sikertelen.");
	}
	~CurlHandle() { curl_easy_cleanup(handle); }
	operator CURL *() const { return handle; }
};

class CurlHeaders final {
	curl_slist *headers = nullptr;

public:
	CurlHeaders() = default;
	CurlHeaders(CurlHeaders const&) = delete;
	CurlHeaders& operator=(CurlHeaders const&) = delete;
	CurlHeaders(CurlHeaders&& other) noexcept : headers(other.headers) {
		other.headers = nullptr;
	}
	CurlHeaders& operator=(CurlHeaders&& other) noexcept {
		if (this == &other) return *this;
		curl_slist_free_all(headers);
		headers = other.headers;
		other.headers = nullptr;
		return *this;
	}
	~CurlHeaders() { curl_slist_free_all(headers); }
	void Add(std::string const& value) {
		auto updated = curl_slist_append(headers, value.c_str());
		if (!updated)
			throw Error("A HTTP fejlécek létrehozása sikertelen.");
		headers = updated;
	}
	operator curl_slist *() const { return headers; }
};

void configure_common(CURL *curl, std::string const& api_key,
	std::string *response, std::atomic_bool *cancelled) {
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Aegisub-Muteki/AI");
	// Long transcription and review requests have no client-side time limit.
	// They remain cancellable through the transfer callback below.
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 0L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	// Never enable verbose curl logging here: it would expose the Authorization
	// header and therefore the user's API key.
	(void)api_key;
}

json::UnknownElement parse_json(std::string const& value) {
	std::istringstream stream(value);
	json::UnknownElement root;
	json::Reader::Read(root, stream);
	return root;
}

template<typename T>
std::string write_json(T const& value) {
	std::ostringstream stream;
	agi::JsonWriter::Write(value, stream);
	return stream.str();
}

std::string string_field(json::Object const& object, std::string_view name,
	std::string fallback = {}) {
	auto it = object.find(name);
	if (it == object.end()) return fallback;
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (json::Exception const&) {
		return fallback;
	}
}

std::string api_error(std::string const& response, long status) {
	try {
		auto root = parse_json(response);
		auto const& object = static_cast<json::Object const&>(root);
		auto it = object.find("error");
		if (it != object.end()) {
			auto const& error = static_cast<json::Object const&>(it->second);
			auto message = string_field(error, "message");
			if (!message.empty()) return message;
		}
	}
	catch (std::exception const&) {
	}
	return agi::format("Az OpenAI API HTTP %d hibával válaszolt.", status);
}

std::string perform(CURL *curl, CurlHeaders const& headers,
	std::atomic_bool *cancelled) {
	std::string response;
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, static_cast<curl_slist *>(headers));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	auto result = curl_easy_perform(curl);
	if (result == CURLE_ABORTED_BY_CALLBACK && cancelled && cancelled->load())
		throw Error("A kérés megszakítva.");
	if (result != CURLE_OK)
		throw Error(agi::format("Hálózati hiba: %s", curl_easy_strerror(result)));

	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (status < 200 || status >= 300)
		throw Error(api_error(response, status));
	return response;
}

CurlHeaders authenticated_headers(std::string const& key, bool json_body) {
	CurlHeaders headers;
	headers.Add("Authorization: Bearer " + key);
	if (json_body)
		headers.Add("Content-Type: application/json");
	return headers;
}

json::Object message_item(std::string const& role, std::string const& content) {
	json::Object message;
	message["role"] = role;
	message["content"] = content;
	return message;
}

json::Object type_schema(char const *type) {
	json::Object schema;
	schema["type"] = type;
	return schema;
}

json::Object line_schema() {
	json::Object properties;
	properties["line_id"] = type_schema("integer");
	properties["japanese"] = type_schema("string");
	properties["romaji"] = type_schema("string");
	json::Object verdict;
	verdict["type"] = "string";
	json::Array verdict_values;
	for (auto value : {"ok", "minor_issue", "major_issue"})
		verdict_values.emplace_back(value);
	verdict["enum"] = std::move(verdict_values);
	properties["verdict"] = std::move(verdict);
	properties["assessment"] = type_schema("string");
	properties["suggested_text"] = type_schema("string");

	json::Object issues;
	issues["type"] = "array";
	issues["items"] = type_schema("string");
	issues["maxItems"] = 3;
	properties["issues"] = std::move(issues);

	json::Array required;
	for (auto name : {"line_id", "japanese", "romaji", "verdict", "assessment", "issues", "suggested_text"})
		required.emplace_back(name);

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object response_schema() {
	json::Object line_array;
	line_array["type"] = "array";
	line_array["items"] = line_schema();

	json::Object properties;
	properties["message"] = type_schema("string");
	properties["lines"] = std::move(line_array);

	json::Array required;
	required.emplace_back("message");
	required.emplace_back("lines");

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object chat_response_schema() {
	json::Object properties;
	properties["message"] = type_schema("string");

	json::Array required;
	required.emplace_back("message");

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object proofread_issue_schema() {
	json::Object category;
	category["type"] = "string";
	json::Array category_values;
	for (auto value : {"spelling", "punctuation", "grammar", "style",
		"repetition", "consistency", "source_mismatch"})
		category_values.emplace_back(value);
	category["enum"] = std::move(category_values);

	json::Object categories;
	categories["type"] = "array";
	categories["items"] = std::move(category);

	json::Object suggestions;
	suggestions["type"] = "array";
	suggestions["items"] = type_schema("string");
	suggestions["maxItems"] = 3;

	json::Object properties;
	properties["line_id"] = type_schema("integer");
	properties["categories"] = std::move(categories);
	properties["explanation"] = type_schema("string");
	properties["suggestions"] = std::move(suggestions);

	json::Array required;
	for (auto name : {"line_id", "categories", "explanation", "suggestions"})
		required.emplace_back(name);

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object proofread_response_schema() {
	json::Object issues;
	issues["type"] = "array";
	issues["items"] = proofread_issue_schema();

	json::Object properties;
	properties["message"] = type_schema("string");
	properties["issues"] = std::move(issues);

	json::Array required;
	required.emplace_back("message");
	required.emplace_back("issues");

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

std::string instructions(std::string const& custom) {
	std::string prompt =
		"You are a professional audiovisual subtitle quality reviewer. Audit the "
		"supplied existing Hungarian subtitle text; do not translate from scratch "
		"when the current subtitle is already correct. Compare current_hu against "
		"the authoritative Japanese transcript and use source_en as a secondary aid. "
		"Check meaning, omissions, additions, tone, natural Hungarian wording, grammar, "
		"and continuity across the scene. The input lines are ordered and belong to "
		"one continuous scene. Input lines are compact arrays in the exact order declared "
		"by columns. Return exactly one record for every supplied line_id "
		"in the same order. verdict must be ok, minor_issue, or major_issue. assessment "
		"must be one short Hungarian sentence and issues must contain at most three short "
		"Hungarian items. suggested_text must be "
		"empty when no change is needed; otherwise it must contain one corrected, plain "
		"Hungarian subtitle without ASS tags or literal newlines. japanese should contain "
		"the Japanese words assigned to the line, and romaji an easy-to-read Hepburn "
		"romanization. During follow-up chat, answer only the user's new question in "
		"Hungarian. Do not repeat the per-line review unless explicitly asked.";
	if (!custom.empty())
		prompt += " Additional user review rules: " + custom;
	return prompt;
}

std::string proofread_instructions(std::string const& custom) {
	std::string prompt =
		"You are a meticulous Hungarian subtitle proofreader. Review only input lines "
		"where target is 1, and use every supplied line in the chronologically ordered "
		"batch as context. Return one combined issue object per flawed "
		"target line and no object for correct lines. Keep false positives very low for "
		"spelling claims. Check Hungarian spelling and typos, unambiguous comma and other "
		"punctuation errors, grammar, awkward or unclear wording in context, repeated words "
		"or phrases, and terminology/name/spelling consistency across the entire subtitle. "
		"Do not flag high CPS, reading speed, line length or subtitle duration issues. "
		"Treat source_line as a semantic reference: flag a contextually wrong but correctly "
		"spelled Hungarian word (for example bab instead of báb) when the source makes the "
		"mistake clear. Never propose a fresh translation merely because source_line differs. "
		"Editorial notes intentionally left in the subtitle are allowed. Never flag the presence, "
		"wording, formatting or retention of an editor/editorial note, and never suggest removing it. "
		"Input lines are compact arrays in the exact order declared by columns. Merge all findings "
		"for the same line into its categories, explanation and alternatives. explanation must be "
		"one short Hungarian sentence. Each suggestion is the complete "
		"replacement ASS text for that line, not merely a changed word. Preserve every existing "
		"ASS override block such as {\\i1} and {\\i0} verbatim in every suggestion and preserve "
		"explicit \\N, \\n and \\h controls unless reflow is necessary. Never add literal newlines. "
		"If categories is exactly [spelling] and the issue is only a clear typo, one strong "
		"suggestion is sufficient. For every other issue return exactly three genuinely useful, "
		"meaning-preserving alternatives, ordered best first. Do not return duplicate alternatives. "
		"If there are no findings, return an empty issues array and a short Hungarian message.";
	if (!custom.empty())
		prompt += " Additional user review rules: " + custom;
	return prompt;
}

std::string extract_output_text(json::Object const& root) {
	auto output_it = root.find("output");
	if (output_it == root.end())
		throw Error("Az OpenAI válaszából hiányzik az output mező.");

	auto const& output = static_cast<json::Array const&>(output_it->second);
	for (auto const& item_value : output) {
		auto const& item = static_cast<json::Object const&>(item_value);
		if (string_field(item, "type") != "message") continue;
		auto content_it = item.find("content");
		if (content_it == item.end()) continue;
		for (auto const& content_value : static_cast<json::Array const&>(content_it->second)) {
			auto const& content = static_cast<json::Object const&>(content_value);
			if (string_field(content, "type") == "output_text") {
				auto text = string_field(content, "text");
				if (!text.empty()) return text;
			}
		}
	}
	throw Error("Az OpenAI nem adott szöveges választ.");
}

ReviewResult parse_review(std::string const& output_text,
	std::string conversation_json) {
	auto parsed = parse_json(output_text);
	auto const& object = static_cast<json::Object const&>(parsed);

	ReviewResult result;
	result.message = string_field(object, "message");
	result.conversation_json = std::move(conversation_json);

	auto lines_it = object.find("lines");
	if (lines_it == object.end())
		throw Error("Az ellenőrzési válaszból hiányzik a lines mező.");

	for (auto const& value : static_cast<json::Array const&>(lines_it->second)) {
		auto const& line = static_cast<json::Object const&>(value);
		LineReview review;
		review.id = static_cast<int>(static_cast<json::Integer const&>(line.at("line_id")));
		review.japanese = string_field(line, "japanese");
		review.romaji = string_field(line, "romaji");
		review.verdict = string_field(line, "verdict");
		review.assessment = string_field(line, "assessment");
		review.suggested_text = string_field(line, "suggested_text");
		auto issues_it = line.find("issues");
		if (issues_it != line.end()) {
			for (auto const& issue : static_cast<json::Array const&>(issues_it->second))
				review.issues.push_back(static_cast<json::String const&>(issue));
		}
		result.lines.push_back(std::move(review));
	}
	return result;
}

ReviewResult parse_chat(std::string const& output_text,
	std::string conversation_json) {
	auto parsed = parse_json(output_text);
	auto const& object = static_cast<json::Object const&>(parsed);
	ReviewResult result;
	result.message = string_field(object, "message");
	result.conversation_json = std::move(conversation_json);
	if (result.message.empty())
		throw Error("Az AI üres chatválaszt adott.");
	return result;
}

ProofreadResult parse_proofread(std::string const& output_text) {
	auto parsed = parse_json(output_text);
	auto const& object = static_cast<json::Object const&>(parsed);
	ProofreadResult result;
	result.message = string_field(object, "message");

	auto issues_it = object.find("issues");
	if (issues_it == object.end())
		throw Error("Az utóellenőrzési válaszból hiányzik az issues mező.");

	for (auto const& value : static_cast<json::Array const&>(issues_it->second)) {
		auto const& object = static_cast<json::Object const&>(value);
		ProofreadIssue issue;
		issue.line_id = static_cast<int>(static_cast<json::Integer const&>(object.at("line_id")));
		issue.explanation = string_field(object, "explanation");
		for (auto const& category : static_cast<json::Array const&>(object.at("categories")))
			issue.categories.push_back(static_cast<json::String const&>(category));
		for (auto const& suggestion : static_cast<json::Array const&>(object.at("suggestions"))) {
			auto text = static_cast<json::String const&>(suggestion);
			if (std::find(issue.suggestions.begin(), issue.suggestions.end(), text) == issue.suggestions.end())
				issue.suggestions.push_back(std::move(text));
		}

		// A partially useful result is preferable to discarding a long whole-file
		// review. The prompt requests three alternatives where appropriate, but a
		// short model response is still safe to present to the user.
		if (issue.suggestions.empty()) continue;
		result.issues.push_back(std::move(issue));
	}
	return result;
}

std::string append_response_items(std::string const& history_json,
	json::Object& response_root) {
	auto history_root = parse_json(history_json);
	auto history = std::move(static_cast<json::Array&>(history_root));
	auto output_it = response_root.find("output");
	if (output_it == response_root.end())
		throw Error("Az OpenAI válaszából hiányzik az output mező.");
	auto& output = static_cast<json::Array&>(output_it->second);
	for (auto& item : output)
		history.push_back(std::move(item));
	return write_json(history);
}

std::string post_json(std::string const& key, std::string const& endpoint,
	std::string const& body, std::atomic_bool *cancelled) {
	CurlHandle curl;
	std::string response;
	configure_common(curl, key, &response, cancelled);
	curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
	auto headers = authenticated_headers(key, true);
	return perform(curl, headers, cancelled);
}

ReviewResult structured_request(std::string const& key,
	std::string const& model, std::string const& custom_instructions,
	std::string history_json, std::string const& user_message,
	std::atomic_bool *cancelled, bool include_line_reviews) {
	auto history_root = parse_json(history_json);
	auto history = std::move(static_cast<json::Array&>(history_root));
	history.emplace_back(message_item("user", user_message));
	auto updated_history_json = write_json(history);

	// Parse the just-serialized history again because Cajun values are move-only
	// and the request takes ownership of the input array.
	auto request_history_root = parse_json(updated_history_json);
	auto request_history = std::move(static_cast<json::Array&>(request_history_root));

	json::Object format;
	format["type"] = "json_schema";
	format["name"] = include_line_reviews ? "subtitle_review" : "subtitle_review_chat";
	format["strict"] = true;
	format["schema"] = include_line_reviews ? response_schema() : chat_response_schema();

	json::Object text;
	text["format"] = std::move(format);
	text["verbosity"] = include_line_reviews ? "low" : "medium";

	json::Object reasoning;
	reasoning["effort"] = "low";

	json::Object request;
	request["model"] = model;
	request["instructions"] = instructions(custom_instructions);
	request["input"] = std::move(request_history);
	request["text"] = std::move(text);
	request["reasoning"] = std::move(reasoning);
	request["store"] = false;
	request["max_output_tokens"] = include_line_reviews ? 12000 : 4000;

	auto response_text = post_json(key, std::string(api_base) + "/responses",
		write_json(request), cancelled);
	auto response_root_value = parse_json(response_text);
	auto& response_root = static_cast<json::Object&>(response_root_value);
	auto output_text = extract_output_text(response_root);
	auto complete_history = append_response_items(updated_history_json, response_root);
	return include_line_reviews
		? parse_review(output_text, std::move(complete_history))
		: parse_chat(output_text, std::move(complete_history));
}

ProofreadResult proofread_request(std::string const& key,
	std::string const& model, std::string const& custom_instructions,
	std::string const& user_message, std::atomic_bool *cancelled) {
	json::Array input;
	input.emplace_back(message_item("user", user_message));

	json::Object format;
	format["type"] = "json_schema";
	format["name"] = "hungarian_subtitle_proofread";
	format["strict"] = true;
	format["schema"] = proofread_response_schema();

	json::Object text;
	text["format"] = std::move(format);
	text["verbosity"] = "low";

	json::Object reasoning;
	reasoning["effort"] = "low";

	json::Object request;
	request["model"] = model;
	request["instructions"] = proofread_instructions(custom_instructions);
	request["input"] = std::move(input);
	request["text"] = std::move(text);
	request["reasoning"] = std::move(reasoning);
	request["store"] = false;
	request["max_output_tokens"] = 16000;

	auto response_text = post_json(key, std::string(api_base) + "/responses",
		write_json(request), cancelled);
	auto response_root_value = parse_json(response_text);
	auto const& response_root = static_cast<json::Object const&>(response_root_value);
	return parse_proofread(extract_output_text(response_root));
}

std::string read_environment_key() {
	auto value = std::getenv("OPENAI_API_KEY");
	return value ? value : "";
}

#ifdef _WIN32
std::string read_credential_key() {
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(credential_target, CRED_TYPE_GENERIC, 0, &credential))
		return {};
	std::string value(reinterpret_cast<char const *>(credential->CredentialBlob),
		credential->CredentialBlobSize);
	CredFree(credential);
	return value;
}

std::string windows_error_message(DWORD code) {
	wchar_t *buffer = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
		reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
	std::wstring wide = buffer ? buffer : L"Ismeretlen Windows hiba";
	if (buffer) LocalFree(buffer);
	return std::string(wide.begin(), wide.end());
}
#endif

} // namespace

OpenAIClient::OpenAIClient(std::string api_key, std::string model,
	std::string transcription_model, std::string custom_instructions,
	std::atomic_bool *cancelled)
: api_key(std::move(api_key))
, model(std::move(model))
, transcription_model(std::move(transcription_model))
, custom_instructions(std::move(custom_instructions))
, cancelled(cancelled) {
	if (this->api_key.empty()) throw Error("Nincs beállítva OpenAI API-kulcs.");
	if (this->model.empty()) throw Error("Nincs beállítva ellenőrzési modell.");
	if (this->transcription_model.empty()) throw Error("Nincs beállítva beszédfelismerési modell.");
}

void OpenAIClient::TestConnection() const {
	CurlHandle curl;
	std::string response;
	configure_common(curl, api_key, &response, cancelled);
	char *escaped = curl_easy_escape(curl, model.c_str(), static_cast<int>(model.size()));
	if (!escaped) throw Error("A modellnév kódolása sikertelen.");
	std::string url = std::string(api_base) + "/models/" + escaped;
	curl_free(escaped);
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	auto headers = authenticated_headers(api_key, false);
	(void)perform(curl, headers, cancelled);
}

std::string OpenAIClient::Transcribe(agi::fs::path const& audio_file) const {
	CurlHandle curl;
	std::string response;
	configure_common(curl, api_key, &response, cancelled);
	curl_easy_setopt(curl, CURLOPT_URL, (std::string(api_base) + "/audio/transcriptions").c_str());

	curl_mime *mime = curl_mime_init(curl);
	if (!mime) throw Error("A hangfeltöltés előkészítése sikertelen.");
	auto cleanup = [&] { curl_mime_free(mime); };

	auto add_text = [&](char const *name, std::string const& value) {
		auto part = curl_mime_addpart(mime);
		curl_mime_name(part, name);
		curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
	};

	add_text("model", transcription_model);
	add_text("response_format", "json");
	add_text("languages[]", "ja");
	auto file_part = curl_mime_addpart(mime);
	curl_mime_name(file_part, "file");
	curl_mime_filedata(file_part, audio_file.string().c_str());
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

	try {
		auto headers = authenticated_headers(api_key, false);
		response = perform(curl, headers, cancelled);
		cleanup();
	}
	catch (...) {
		cleanup();
		throw;
	}

	try {
		auto parsed = parse_json(response);
		auto const& root = static_cast<json::Object const&>(parsed);
		auto text = string_field(root, "text");
		if (text.empty()) throw Error("A beszédfelismerés üres eredményt adott.");
		return text;
	}
	catch (Error const&) { throw; }
	catch (std::exception const& error) {
		throw Error(agi::format("A beszédfelismerési válasz nem értelmezhető: %s", error.what()));
	}
}

ReviewResult OpenAIClient::Review(std::vector<SubtitleLine> const& lines,
	std::string const& japanese_transcript) const {
	json::Array input_lines;
	for (auto const& line : lines) {
		json::Array row;
		row.emplace_back(line.id);
		row.emplace_back(line.source_text);
		row.emplace_back(line.current_text);
		row.emplace_back(line.actor);
		input_lines.emplace_back(std::move(row));
	}

	json::Object context;
	context["japanese_transcript"] = japanese_transcript;
	context["review_language"] = "hu";
	context["task"] = "quality_review_existing_subtitles";
	json::Array columns;
	for (auto name : {"line_id", "source_en", "current_hu", "actor"})
		columns.emplace_back(name);
	context["columns"] = std::move(columns);
	context["lines"] = std::move(input_lines);
	return structured_request(api_key, model, custom_instructions, "[]",
		write_json(context), cancelled, true);
}

ReviewResult OpenAIClient::Continue(ReviewResult const& previous,
	std::string const& user_message) const {
	if (user_message.empty()) throw Error("A chatüzenet nem lehet üres.");
	auto result = structured_request(api_key, model, custom_instructions,
		previous.conversation_json, user_message, cancelled, false);
	result.lines = previous.lines;
	return result;
}

ProofreadResult OpenAIClient::Proofread(std::vector<SubtitleLine> const& lines) const {
	auto make_context = [](std::vector<SubtitleLine const *> const& batch) {
		json::Array input_lines;
		for (auto line : batch) {
			json::Array row;
			row.emplace_back(line->id);
			row.emplace_back(line->target ? 1 : 0);
			row.emplace_back(line->source_text);
			row.emplace_back(line->ass_text);
			row.emplace_back(line->actor);
			input_lines.emplace_back(std::move(row));
		}

		json::Object context;
		context["language"] = "hu";
		context["task"] = "hungarian_subtitle_proofread";
		json::Array columns;
		for (auto name : {"line_id", "target", "source_line", "ass_text", "actor"})
			columns.emplace_back(name);
		context["columns"] = std::move(columns);
		context["lines"] = std::move(input_lines);
		return write_json(context);
	};

	ProofreadResult combined;
	std::vector<SubtitleLine const *> batch;
	size_t estimated_chars = 256;
	size_t request_count = 0;
	auto send_batch = [&] {
		if (batch.empty()) return;
		auto message = make_context(batch);
		if (message.size() > proofread_max_input_chars)
			throw Error("Az AI-utóellenőrzés egyik feliratsora túl hosszú a feldolgozáshoz.");
		auto part = proofread_request(api_key, model, custom_instructions,
			message, cancelled);
		if (request_count++ == 0)
			combined.message = std::move(part.message);
		combined.issues.insert(combined.issues.end(),
			std::make_move_iterator(part.issues.begin()),
			std::make_move_iterator(part.issues.end()));
		batch.clear();
		estimated_chars = 256;
	};

	for (auto const& line : lines) {
		json::Array row;
		row.emplace_back(line.id);
		row.emplace_back(line.target ? 1 : 0);
		row.emplace_back(line.source_text);
		row.emplace_back(line.ass_text);
		row.emplace_back(line.actor);
		auto row_chars = write_json(row).size() + 1;
		if (row_chars + 256 > proofread_max_input_chars)
			throw Error("Az AI-utóellenőrzés egyik feliratsora túl hosszú a feldolgozáshoz.");
		if (!batch.empty() && (batch.size() >= proofread_max_lines_per_request ||
			estimated_chars + row_chars > proofread_max_input_chars))
			send_batch();
		batch.push_back(&line);
		estimated_chars += row_chars;
	}
	send_batch();
	if (request_count > 1)
		combined.message.clear();
	return combined;
}

std::string GetApiKey() {
	{
		std::lock_guard<std::mutex> lock(session_key_mutex);
		if (!session_key.empty()) return session_key;
	}
	auto environment = read_environment_key();
	if (!environment.empty()) return environment;
#ifdef _WIN32
	return read_credential_key();
#else
	return {};
#endif
}

ApiKeySource GetApiKeySource() {
	{
		std::lock_guard<std::mutex> lock(session_key_mutex);
		if (!session_key.empty()) return ApiKeySource::Session;
	}
	if (!read_environment_key().empty()) return ApiKeySource::Environment;
#ifdef _WIN32
	if (!read_credential_key().empty()) return ApiKeySource::CredentialManager;
#endif
	return ApiKeySource::None;
}

void SetSessionApiKey(std::string key) {
	std::lock_guard<std::mutex> lock(session_key_mutex);
	session_key = std::move(key);
}

void ClearSessionApiKey() {
	std::lock_guard<std::mutex> lock(session_key_mutex);
	std::fill(session_key.begin(), session_key.end(), '\0');
	session_key.clear();
}

bool StoreApiKey(std::string const& key, std::string *error) {
#ifdef _WIN32
	if (key.empty()) {
		if (error) *error = "Az API-kulcs nem lehet üres.";
		return false;
	}
	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = const_cast<wchar_t *>(credential_target);
	credential.CredentialBlobSize = static_cast<DWORD>(key.size());
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(key.data()));
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	credential.UserName = const_cast<wchar_t *>(L"OpenAI API");
	if (!CredWriteW(&credential, 0)) {
		if (error) *error = windows_error_message(GetLastError());
		return false;
	}
	SetSessionApiKey(key);
	return true;
#else
	if (error) *error = "A biztonságos mentés ezen a platformon még nem támogatott; használja az OPENAI_API_KEY környezeti változót.";
	return false;
#endif
}

bool DeleteStoredApiKey(std::string *error) {
	ClearSessionApiKey();
#ifdef _WIN32
	if (CredDeleteW(credential_target, CRED_TYPE_GENERIC, 0)) return true;
	auto code = GetLastError();
	if (code == ERROR_NOT_FOUND) return true;
	if (error) *error = windows_error_message(code);
	return false;
#else
	(void)error;
	return true;
#endif
}

bool HasStoredApiKey() {
#ifdef _WIN32
	return !read_credential_key().empty();
#else
	return false;
#endif
}

} // namespace ai
