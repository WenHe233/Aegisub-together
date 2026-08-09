#include "compat.h"
#include "muteki_update.h"

#include "command/command.h"
#include "include/aegisub/context.h"
#include "main.h"
#include "options.h"
#include "project.h"
#include "subs_controller.h"
#include "version.h"

#include <libaegisub/path.h>
#include <libaegisub/signal.h>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace muteki_update {
namespace {

namespace fs = std::filesystem;

/// Where the changelogs are read from, compiled in rather than configurable.
///
/// It used to be an option, which turned out to be a trap: Options::Flush writes
/// every setting to config.json, so a copy of the address was pinned in every
/// installation and a later default could never reach anyone. If the address had
/// stopped working, those installations would have had no way of finding updates
/// again. A constant cannot be pinned.
///
/// "HEAD" rather than a branch name: raw.githubusercontent.com resolves it to
/// whatever the repository's default branch is, so renaming the branch, or
/// pointing the default at another one, does not break the address.
constexpr char CHANGELOG_BASE[] = "https://raw.githubusercontent.com/croni1012/Aegisub/HEAD";

/// The automation package is served by a script rather than being a static file,
/// so it is the one thing that cannot live in the repository beside the rest.
constexpr char AUTOMATION_URL[] = "https://mutekifansub.hu/public/nyaa/Aegisub/muteki-update.php";

constexpr char CHANGELOG_FALLBACK_LANGUAGE[] = "en";
/// What the "View as" chooser starts on: the language the changelog is written in.
constexpr char CHANGELOG_DEFAULT_LANGUAGE[] = "hu";

/// Changelogs are published per language under changelog/, so the text is
/// translated once and shipped rather than translated on every view.
std::string ChangelogName(std::string const& language) {
	return "changelog/" + language + ".txt";
}

std::string FetchChangelog(std::string const& language);

class UpdateError final : public std::runtime_error {
public:
	explicit UpdateError(std::string message) : std::runtime_error(std::move(message)) { }
};

struct Release {
	std::string version;
	std::string package;
	std::string changes;
};

struct PendingUpdate {
	fs::path helper;
	fs::path stage;
	fs::path destination;
	fs::path restart_exe;
	std::vector<fs::path> restart_files;
};

std::optional<PendingUpdate> pending_update;

std::string Trim(std::string value) {
	auto not_space = [](unsigned char c) { return !std::isspace(c); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

std::string ChangelogUrl(std::string const& language) {
	return std::string(CHANGELOG_BASE) + "/" + ChangelogName(language);
}

size_t AppendString(char *data, size_t size, size_t count, void *target) {
	auto bytes = size * count;
	static_cast<std::string *>(target)->append(data, bytes);
	return bytes;
}

void ConfigureCurl(CURL *curl, std::string const& url) {
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Aegisub-Muteki/muteki-update");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifdef CURLOPT_PROTOCOLS_STR
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
}

void CheckHttpResult(CURL *curl, CURLcode result, std::string const& resource) {
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (result != CURLE_OK)
		throw UpdateError("Hálózati hiba: " + std::string(curl_easy_strerror(result)));
	if (status == 404)
		throw UpdateError("A kért fájl jelenleg nem létezik: " + resource);
	if (status < 200 || status >= 300)
		throw UpdateError("A kiszolgáló HTTP " + std::to_string(status) + " hibával válaszolt.");
}

std::string FetchText(std::string const& url) {
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
	if (!curl) throw UpdateError("A hálózati kapcsolat nem inicializálható.");
	std::string result;
	ConfigureCurl(curl.get(), url);
	curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, AppendString);
	curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &result);
	auto code = curl_easy_perform(curl.get());
	CheckHttpResult(curl.get(), code, url);
	return result;
}

struct DownloadTarget {
	FILE *file = nullptr;
	wxProgressDialog *progress = nullptr;
	bool cancelled = false;
};

size_t WriteFile(char *data, size_t size, size_t count, void *raw_target) {
	auto target = static_cast<DownloadTarget *>(raw_target);
	return std::fwrite(data, 1, size * count, target->file);
}

int DownloadProgress(void *raw_target, curl_off_t total, curl_off_t current, curl_off_t, curl_off_t) {
	auto target = static_cast<DownloadTarget *>(raw_target);
	int value = total > 0 ? static_cast<int>(std::min<curl_off_t>(1000, current * 1000 / total)) : 0;
	bool keep_going = target->progress->Update(value);
	if (!keep_going) target->cancelled = true;
	return keep_going ? 0 : 1;
}

void DownloadFile(std::string const& url, fs::path const& destination, wxWindow *parent) {
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
	if (!curl) throw UpdateError("A hálózati kapcsolat nem inicializálható.");
	FILE *raw_file = nullptr;
#ifdef _WIN32
	if (_wfopen_s(&raw_file, destination.c_str(), L"wb") != 0)
#else
	raw_file = std::fopen(destination.c_str(), "wb");
#endif
	if (!raw_file) throw UpdateError("Az ideiglenes letöltési fájl nem hozható létre.");
	std::unique_ptr<FILE, decltype(&std::fclose)> file(raw_file, std::fclose);

	wxProgressDialog progress(_("Muteki update"), _("Downloading update package..."), 1000, parent,
		wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
	DownloadTarget target{file.get(), &progress, false};
	ConfigureCurl(curl.get(), url);
	curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteFile);
	curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &target);
	curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, DownloadProgress);
	curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &target);
	auto code = curl_easy_perform(curl.get());
	file.reset();
	if (target.cancelled) throw UpdateError("A letöltés megszakítva.");
	CheckHttpResult(curl.get(), code, url);
}

std::vector<int> VersionParts(std::string version) {
	if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) version.erase(version.begin());
	std::vector<int> parts;
	std::stringstream stream(version);
	std::string part;
	while (std::getline(stream, part, '.')) {
		try { parts.push_back(std::stoi(part)); }
		catch (...) { parts.push_back(0); }
	}
	return parts;
}

int CompareVersions(std::string const& left, std::string const& right) {
	auto a = VersionParts(left);
	auto b = VersionParts(right);
	auto count = std::max(a.size(), b.size());
	a.resize(count);
	b.resize(count);
	if (a < b) return -1;
	if (b < a) return 1;
	return 0;
}

/// The package line of a release, kept as the address it is.
///
/// It used to be cut down to a bare file name and re-attached to one configured
/// base, which assumed every package sits in the same directory. Release assets do
/// not: each one is under its own tag. Keeping the whole address also means the
/// changelog itself says where each version lives, so a future move needs no new
/// program.
///
/// Nothing is derived from this but the request; the download always lands in
/// package.zip inside a fresh temporary directory, so there is no file name here to
/// be traversed with. Only the scheme has to be insisted on.
std::string PackageUrl(std::string value, std::string const& version) {
	value = Trim(std::move(value));
	if (value.empty())
		throw UpdateError("A changelog " + version + " kiadásához nem tartozik letöltési cím.");
	if (!value.starts_with("https://"))
		throw UpdateError("A changelogban szereplő letöltési címnek HTTPS-nek kell lennie: " + value);
	return value;
}

/// The version a release header announces, or "" if the line is not a header.
///
/// A header carries the word for "version" and the number, in whichever order that
/// language puts them, before a trailing "---". The version is the last word
/// containing a digit: languages disagree about the order, and Basque came back as
/// "3.5.2 bertsioa ---", which a rule looking only at the last word dropped
/// silently. Keep in step with header_version in tools/release/release_common.py.
std::string HeaderVersion(std::string const& line) {
	auto trimmed = Trim(line);
	if (!trimmed.ends_with("---")) return {};

	auto head = Trim(trimmed.substr(0, trimmed.size() - 3));
	std::istringstream words(head);
	std::string word;
	std::string version;
	while (words >> word) {
		if (std::any_of(word.begin(), word.end(),
				[](unsigned char c) { return std::isdigit(c); }))
			version = word;
	}
	return version;
}

/// The changelog with the package line taken out of every release.
///
/// The line after a header is the download URL, which is there for the installer
/// and is only noise to read. The dialogs that show the file itself go through this
/// so that they agree with the update dialog, which builds its text from the parsed
/// releases and so never had the URL in it.
std::string ChangelogForDisplay(std::string const& text) {
	std::string out;
	std::istringstream input(text);
	std::string line;
	bool drop_package = false;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!HeaderVersion(line).empty()) {
			drop_package = true;
		}
		else if (drop_package) {
			// Only the first non-blank line after the header is the package.
			if (Trim(line).empty()) {
				out += line;
				out += '\n';
				continue;
			}
			drop_package = false;
			continue;
		}
		out += line;
		out += '\n';
	}
	return out;
}

std::vector<Release> ParseChangelog(std::string const& text) {
	std::vector<Release> releases;
	std::optional<Release> current;
	bool need_package = false;
	std::istringstream input(text);
	std::string line;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		auto trimmed = Trim(line);
		std::string version = HeaderVersion(line);
		if (!version.empty()) {
			if (current) {
				current->package = PackageUrl(current->package, current->version);
				current->changes = Trim(current->changes);
				releases.push_back(std::move(*current));
			}
			current = Release{version, {}, {}};
			need_package = true;
			continue;
		}
		if (!current) continue;
		if (need_package && !trimmed.empty()) {
			current->package = trimmed;
			need_package = false;
			continue;
		}
		if (!current->changes.empty()) current->changes += '\n';
		current->changes += line;
	}
	if (current) {
		current->package = PackageUrl(current->package, current->version);
		current->changes = Trim(current->changes);
		releases.push_back(std::move(*current));
	}
	if (releases.empty()) throw UpdateError("A changelog nem tartalmaz felismerhető verziót.");
	return releases;
}

Release const& LatestRelease(std::vector<Release> const& releases) {
	return *std::max_element(releases.begin(), releases.end(), [](Release const& a, Release const& b) {
		return CompareVersions(a.version, b.version) < 0;
	});
}

std::string ChangesBetween(std::vector<Release> const& releases, std::string const& installed,
	std::string const& target) {
	std::vector<Release const *> included;
	for (auto const& release : releases) {
		if (CompareVersions(release.version, installed) > 0 &&
			CompareVersions(release.version, target) <= 0)
			included.push_back(&release);
	}
	std::sort(included.begin(), included.end(), [](Release const *a, Release const *b) {
		return CompareVersions(a->version, b->version) > 0;
	});

	std::string changes;
	for (auto const *release : included) {
		if (!changes.empty()) changes += "\n\n";
		changes += "Verzió " + release->version;
		if (!release->changes.empty()) changes += "\n" + release->changes;
	}
	return changes;
}

/// Fetch the changelog for `language`, falling back to English when there is no
/// translation published for it.
std::string FetchChangelog(std::string const& language) {
	if (language != CHANGELOG_FALLBACK_LANGUAGE) {
		try { return FetchText(ChangelogUrl(language)); }
		catch (std::exception const&) { }
	}
	return FetchText(ChangelogUrl(CHANGELOG_FALLBACK_LANGUAGE));
}

/// Languages the changelog is published in. Anything missing on the server falls
/// back to English, so listing one that has no file yet is harmless.
std::vector<std::pair<std::string, wxString>> ChangelogLanguages() {
	// The native names have to go through FromUTF8: building a wxString straight
	// from a narrow literal decodes it in the current locale, which turns every
	// non-Latin name into mojibake.
	auto native = [](char const *utf8) { return wxString::FromUTF8(utf8); };
	return {
		{"en", native("English")}, {"hu", native("Magyar")},
		{"ar", native("العربية")}, {"be", native("Беларуская")},
		{"bg", native("Български")}, {"ca", native("Català")},
		{"cs", native("Čeština")}, {"da", native("Dansk")},
		{"de", native("Deutsch")}, {"el", native("Ελληνικά")},
		{"es", native("Español")}, {"eu", native("Euskara")},
		{"fa", native("فارسی")}, {"fi", native("Suomi")},
		{"fr_FR", native("Français")}, {"gl", native("Galego")},
		{"id", native("Bahasa Indonesia")}, {"it", native("Italiano")},
		{"ja", native("日本語")}, {"ko", native("한국어")},
		{"nl", native("Nederlands")}, {"pl", native("Polski")},
		{"pt_BR", native("Português (BR)")}, {"pt_PT", native("Português (PT)")},
		{"ru", native("Русский")}, {"sr_RS", native("Српски")},
		{"sr_RS@latin", native("Srpski")}, {"tr", native("Türkçe")},
		{"uk_UA", native("Українська")}, {"vi", native("Tiếng Việt")},
		{"zh_CN", native("简体中文")}, {"zh_TW", native("繁體中文")},
	};
}

/// A "View as ..." chooser for a changelog view. Each language is a published
/// file rather than a translation made on the spot, so switching is a download
/// instead of a wait on a model.
wxSizer *MakeChangelogLanguageRow(wxDialog *dialog, wxTextCtrl *target,
	std::string shown_language) {
	auto row = new wxBoxSizer(wxHORIZONTAL);
	row->Add(new wxStaticText(dialog, -1, _("View as")), 0,
		wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

	auto languages = std::make_shared<std::vector<std::pair<std::string, wxString>>>(
		ChangelogLanguages());
	wxArrayString choices;
	for (auto const& [code, label] : *languages) choices.Add(label);
	auto choice = new wxChoice(dialog, -1, wxDefaultPosition, wxDefaultSize, choices);

	int selection = 0;
	for (size_t i = 0; i < languages->size(); ++i)
		if ((*languages)[i].first == shown_language) selection = static_cast<int>(i);
	choice->SetSelection(selection);
	row->Add(choice, 0, wxALIGN_CENTER_VERTICAL);

	choice->Bind(wxEVT_CHOICE, [dialog, target, choice, languages](wxCommandEvent&) {
		int selected = choice->GetSelection();
		if (selected < 0 || selected >= static_cast<int>(languages->size())) return;
		wxBusyCursor busy;
		try {
			target->SetValue(to_wx(ChangelogForDisplay(FetchChangelog((*languages)[selected].first))));
		}
		catch (std::exception const& error) {
			wxMessageBox(to_wx(error.what()), _("Muteki update error"), wxOK | wxICON_ERROR, dialog);
		}
	});
	return row;
}

bool ConfirmInstall(wxWindow *parent, Release const& release, std::string const& all_changes) {
	wxDialog dialog(parent, -1, _("Confirm update"), wxDefaultPosition, wxDefaultSize,
		wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
	auto outer = new wxBoxSizer(wxVERTICAL);
	auto summary = wxString::Format(_("Installed version: %s\nSelected version: %s\n\nChanges:"),
		to_wx(GetMutekiVersionString()), to_wx(release.version));
	outer->Add(new wxStaticText(&dialog, -1, summary), 0, wxEXPAND | wxALL, 10);
	auto changes = new wxTextCtrl(&dialog, -1,
		all_changes.empty() ? _("No change description is available.") : to_wx(all_changes),
		wxDefaultPosition, wxSize(600, 260), wxTE_MULTILINE | wxTE_READONLY);
	outer->Add(changes, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
	outer->Add(new wxStaticText(&dialog, -1, _("Do you want to download and install this version?")),
		0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
	auto buttons = new wxBoxSizer(wxHORIZONTAL);
	buttons->Add(MakeChangelogLanguageRow(&dialog, changes, CHANGELOG_DEFAULT_LANGUAGE), 0,
		wxALIGN_CENTER_VERTICAL);
	buttons->AddStretchSpacer();
	buttons->Add(dialog.CreateStdDialogButtonSizer(wxYES | wxNO), 0, wxALIGN_CENTER_VERTICAL);
	outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

	// Without this, declining the update does not work and Escape installs it.
	// CreateStdDialogButtonSizer makes Yes the affirmative id but leaves the escape
	// id at wxID_ANY, and wxDialogBase then treats No as an unknown button - the
	// click is skipped and nothing happens - while Escape looks for a Cancel button,
	// finds none, and falls back to the affirmative id, which is Yes.
	dialog.SetEscapeId(wxID_NO);

	dialog.SetSizerAndFit(outer);
	dialog.SetMinSize(wxSize(620, 430));
	dialog.CentreOnParent();
	return dialog.ShowModal() == wxID_YES;
}

fs::path MakeTempRoot() {
#ifdef _WIN32
	auto pid = GetCurrentProcessId();
#else
	auto pid = 0;
#endif
	auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	auto root = fs::temp_directory_path() / ("muteki-update-" + std::to_string(pid) + "-" + std::to_string(stamp));
	fs::create_directories(root);
	return root;
}

bool SafeRelativePath(fs::path const& path) {
	if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
	for (auto const& part : path)
		if (part == "..") return false;
	return true;
}

void ExtractZip(fs::path const& archive, fs::path const& destination) {
	wxFFileInputStream file(archive.wstring());
	if (!file.IsOk()) throw UpdateError("A letöltött ZIP nem nyitható meg.");
	wxZipInputStream zip(file);
	fs::create_directories(destination);
	size_t entries = 0;
	unsigned long long total_size = 0;
	while (auto raw_entry = zip.GetNextEntry()) {
		std::unique_ptr<wxZipEntry> entry(raw_entry);
		if (++entries > 20000) throw UpdateError("A ZIP túl sok fájlt tartalmaz.");
		auto name = entry->GetName();
#ifdef _WIN32
		fs::path relative(name.ToStdWstring());
#else
		fs::path relative(from_wx(name));
#endif
		relative = relative.lexically_normal();
		if (!SafeRelativePath(relative)) throw UpdateError("A ZIP nem biztonságos elérési utat tartalmaz.");
		auto output = destination / relative;
		if (entry->IsDir()) {
			fs::create_directories(output);
			continue;
		}
		auto size = entry->GetSize();
		if (size != wxInvalidOffset) {
			total_size += static_cast<unsigned long long>(size);
			if (total_size > 2ULL * 1024 * 1024 * 1024) throw UpdateError("A ZIP kibontott mérete túl nagy.");
		}
		fs::create_directories(output.parent_path());
		wxFFileOutputStream out(output.wstring());
		if (!out.IsOk()) throw UpdateError("Nem hozható létre egy kibontott fájl: " + output.string());
		zip.Read(out);
		if (!out.IsOk()) throw UpdateError("A ZIP sérült vagy hiányos.");
	}
	if (entries == 0) throw UpdateError("A letöltött ZIP üres.");
}

std::vector<fs::path> PackageFiles(fs::path const& stage) {
	std::vector<fs::path> files;
	for (auto const& entry : fs::recursive_directory_iterator(stage)) {
		if (!entry.is_regular_file()) continue;
		auto relative = fs::relative(entry.path(), stage);
		if (!SafeRelativePath(relative)) throw UpdateError("Érvénytelen fájl található a csomagban.");
		files.push_back(std::move(relative));
	}
	std::sort(files.begin(), files.end());
	return files;
}

void RemoveProtectedUserData(fs::path const& stage) {
	static constexpr const char *protected_entries[] = {
		"app.log", "autoback", "automation", "autosave", "cache", "catalog",
		"config.json", "crashdumps", "font_cache.json", "font_list.json", "hotkey.json",
		"log", "mru.json", "recovered", "shift_history.json"
	};
	for (auto name : protected_entries) {
		std::error_code ignored;
		fs::remove_all(stage / name, ignored);
	}
}

bool ReplaceFileFromStage(fs::path const& source, fs::path const& destination, std::string& error) {
	std::error_code ec;
	fs::create_directories(destination.parent_path(), ec);
	if (ec) { error = ec.message(); return false; }
	auto temporary = destination;
	temporary += ".muteki-update-new";
	fs::remove(temporary, ec);
	ec.clear();
	fs::copy_file(source, temporary, fs::copy_options::overwrite_existing, ec);
	if (ec) { error = ec.message(); return false; }
#ifdef _WIN32
	if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		error = "Windows error " + std::to_string(GetLastError());
		fs::remove(temporary, ec);
		return false;
	}
#else
	fs::rename(temporary, destination, ec);
	if (ec) { error = ec.message(); fs::remove(temporary); return false; }
#endif
	return true;
}

bool ApplyPackage(fs::path const& stage, fs::path const& destination, std::string& error) {
	auto backup = stage.parent_path() / "backup";
	std::error_code ec;
	fs::remove_all(backup, ec);
	fs::create_directories(backup, ec);
	if (ec) { error = ec.message(); return false; }
	std::vector<fs::path> installed;
	std::vector<fs::path> existing;
	try {
		for (auto const& relative : PackageFiles(stage)) {
			auto source = stage / relative;
			auto target = destination / relative;
			if (fs::exists(target)) {
				auto saved = backup / relative;
				fs::create_directories(saved.parent_path());
				fs::copy_file(target, saved, fs::copy_options::overwrite_existing);
				existing.push_back(relative);
			}
			if (!ReplaceFileFromStage(source, target, error)) throw std::runtime_error(error);
			installed.push_back(relative);
		}
		return true;
	}
	catch (std::exception const& e) {
		error = e.what();
		for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
			auto target = destination / *it;
			if (std::find(existing.begin(), existing.end(), *it) != existing.end()) {
				std::string ignored;
				ReplaceFileFromStage(backup / *it, target, ignored);
			}
			else fs::remove(target, ec);
		}
		return false;
	}
}

fs::path ExecutablePath() {
#ifdef _WIN32
	std::wstring result(1024, L'\0');
	for (;;) {
		auto length = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
		if (!length) throw UpdateError("A program elérési útja nem állapítható meg.");
		if (length < result.size() - 1) { result.resize(length); return fs::path(result); }
		result.resize(result.size() * 2);
	}
#else
	return config::path->Decode("?data/aegisub");
#endif
}

#ifdef _WIN32
std::wstring Quote(fs::path const& value) {
	std::wstring out = L"\"";
	unsigned backslashes = 0;
	for (wchar_t c : value.wstring()) {
		if (c == L'\\') { ++backslashes; continue; }
		if (c == L'\"') out.append(backslashes * 2 + 1, L'\\');
		else out.append(backslashes, L'\\');
		backslashes = 0;
		out.push_back(c);
	}
	out.append(backslashes * 2, L'\\');
	out.push_back(L'\"');
	return out;
}

bool StartProcess(fs::path const& exe, std::wstring arguments, fs::path const& working_directory) {
	std::wstring command = Quote(exe) + std::move(arguments);
	STARTUPINFOW startup{sizeof(startup)};
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
		working_directory.c_str(), &startup, &process)) return false;
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return true;
}
#endif

void StageProgramUpdate(Release const& release, wxWindow *parent) {
	auto root = MakeTempRoot();
	try {
		auto archive = root / "package.zip";
		auto stage = root / "package";
		DownloadFile(release.package, archive, parent);
		ExtractZip(archive, stage);
		RemoveProtectedUserData(stage);
		if (!fs::is_regular_file(stage / "aegisub.exe"))
			throw UpdateError("A frissítési csomag nem tartalmaz aegisub.exe fájlt.");
		fs::remove(archive);
		auto current_exe = ExecutablePath();
		auto helper = root / "muteki-update-helper.exe";
		fs::copy_file(current_exe, helper, fs::copy_options::overwrite_existing);
		pending_update = PendingUpdate{helper, stage, current_exe.parent_path(), current_exe, {}};
	}
	catch (...) {
		std::error_code ignored;
		fs::remove_all(root, ignored);
		throw;
	}
}

void CancelPendingUpdate() {
	if (!pending_update) return;
	std::error_code ignored;
	fs::remove_all(pending_update->stage.parent_path(), ignored);
	pending_update.reset();
}

std::vector<fs::path> RestartFiles(agi::Context *context) {
	std::vector<fs::path> files;
	auto add = [&](fs::path const& file) {
		std::error_code ec;
		if (!file.empty() && fs::is_regular_file(file, ec) &&
			std::find(files.begin(), files.end(), file) == files.end())
			files.push_back(file);
	};
	add(context->subsController->Filename());
	add(context->project->VideoName());
	add(context->project->AudioName());
	return files;
}

void InstallRelease(agi::Context *context, Release const& release, std::string const& changes,
	bool already_confirmed = false) {
	if (!already_confirmed && !ConfirmInstall(context->parent, release, changes)) return;
	StageProgramUpdate(release, context->parent);
	pending_update->restart_files = RestartFiles(context);
	agi::signal::Connection file_saved(context->subsController->AddFileSaveListener([context] {
		if (pending_update)
			pending_update->restart_files = RestartFiles(context);
	}));
	if (!wxGetApp().CloseAll()) {
		CancelPendingUpdate();
		wxMessageBox(_("The update was cancelled because Aegisub could not be closed."),
			_("Muteki update"), wxOK | wxICON_INFORMATION);
	}
}

std::pair<std::string, std::vector<Release>> LoadReleases() {
	// Version numbers and package URLs have to survive translation; if a localised
	// file cannot be parsed, English decides what is installable.
	auto text = FetchChangelog(CHANGELOG_DEFAULT_LANGUAGE);
	try { return {text, ParseChangelog(text)}; }
	catch (std::exception const&) { }
	auto fallback = FetchText(ChangelogUrl(CHANGELOG_FALLBACK_LANGUAGE));
	return {fallback, ParseChangelog(fallback)};
}

void ShowError(wxWindow *parent, std::exception const& error) {
	wxMessageBox(to_wx(error.what()), _("Muteki update error"), wxOK | wxICON_ERROR, parent);
}

} // namespace

bool RunHelperMode(wxArrayString const& args) {
#ifndef _WIN32
	return false;
#else
	if (args.size() < 6 || args[1] != "--muteki-update-helper") return false;
	unsigned long pid = 0;
	if (!args[2].ToULong(&pid)) return true;
	fs::path stage(args[3].ToStdWstring());
	fs::path destination(args[4].ToStdWstring());
	fs::path restart(args[5].ToStdWstring());
	if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
		WaitForSingleObject(process, 5 * 60 * 1000);
		CloseHandle(process);
	}
	std::string error;
	bool success = ApplyPackage(stage, destination, error);
	if (!success)
		MessageBoxW(nullptr, to_wx("A frissítés telepítése sikertelen:\n" + error).wc_str(),
			L"Muteki update", MB_OK | MB_ICONERROR);
	std::wstring restart_arguments;
	for (size_t i = 6; i < args.size(); ++i)
		restart_arguments += L" " + Quote(fs::path(args[i].ToStdWstring()));
	if (!StartProcess(restart, restart_arguments, restart.parent_path()))
		MessageBoxW(nullptr, L"Az Aegisub nem indítható újra.", L"Muteki update", MB_OK | MB_ICONERROR);
	std::error_code ignored;
	fs::remove_all(stage, ignored);
	fs::remove_all(stage.parent_path() / "backup", ignored);
	auto self = ExecutablePath();
	MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
	MoveFileExW(self.parent_path().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
	return true;
#endif
}

void LaunchPendingUpdate() {
	if (!pending_update) return;
#ifdef _WIN32
	auto update = std::move(*pending_update);
	pending_update.reset();
	std::wstring arguments = L" --muteki-update-helper " + std::to_wstring(GetCurrentProcessId()) +
		L" " + Quote(update.stage) + L" " + Quote(update.destination) + L" " + Quote(update.restart_exe);
	for (auto const& file : update.restart_files)
		arguments += L" " + Quote(file);
	if (!StartProcess(update.helper, arguments, update.helper.parent_path()))
		MessageBoxW(nullptr, L"A frissítési segédfolyamat nem indítható el.", L"Muteki update", MB_OK | MB_ICONERROR);
#endif
}

void CheckForUpdates(agi::Context *context) {
	try {
		auto [text, releases] = LoadReleases();
		auto const& latest = LatestRelease(releases);
		if (CompareVersions(latest.version, GetMutekiVersionString()) <= 0) {
			wxMessageBox(wxString::Format(_("No newer version is available.\n\nInstalled version: %s\nLatest published version: %s"),
				to_wx(GetMutekiVersionString()), to_wx(latest.version)), _("Muteki update"),
				wxOK | wxICON_INFORMATION, context->parent);
			return;
		}
		InstallRelease(context, latest, ChangesBetween(releases, GetMutekiVersionString(), latest.version));
	}
	catch (std::exception const& e) { ShowError(context->parent, e); }
}

void InstallSelectedVersion(agi::Context *context) {
	try {
		auto [text, releases] = LoadReleases();
		wxArrayString choices;
		for (auto const& release : releases) choices.Add(to_wx(release.version));
		wxSingleChoiceDialog dialog(context->parent, _("Select the version to install:"),
			_("Install a specific version"), choices);
		if (dialog.ShowModal() != wxID_OK) return;
		auto index = static_cast<size_t>(dialog.GetSelection());
		auto comparison = CompareVersions(releases[index].version, GetMutekiVersionString());
		if (comparison < 0) {
			if (wxMessageBox(_("The selected version is older than the installed version. Do you want to continue?"),
				_("Confirm downgrade"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, context->parent) != wxYES) return;
			InstallRelease(context, releases[index], {}, true);
		}
		else {
			auto changes = comparison > 0
				? ChangesBetween(releases, GetMutekiVersionString(), releases[index].version)
				: releases[index].changes;
			InstallRelease(context, releases[index], changes);
		}
	}
	catch (std::exception const& e) { ShowError(context->parent, e); }
}

void ShowChangelog(agi::Context *context) {
	try {
		auto text = FetchChangelog(CHANGELOG_DEFAULT_LANGUAGE);
		wxDialog dialog(context->parent, -1, _("Aegisub changelog"), wxDefaultPosition, wxSize(720, 560),
			wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
		auto sizer = new wxBoxSizer(wxVERTICAL);
		auto view = new wxTextCtrl(&dialog, -1, to_wx(ChangelogForDisplay(text)), wxDefaultPosition, wxDefaultSize,
			wxTE_MULTILINE | wxTE_READONLY);
		sizer->Add(view, 1, wxEXPAND | wxALL, 8);
		auto buttons = new wxBoxSizer(wxHORIZONTAL);
		buttons->Add(MakeChangelogLanguageRow(&dialog, view, CHANGELOG_DEFAULT_LANGUAGE), 0, wxALIGN_CENTER_VERTICAL);
		buttons->AddStretchSpacer();
		buttons->Add(dialog.CreateStdDialogButtonSizer(wxOK), 0, wxALIGN_CENTER_VERTICAL);
		sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
		dialog.SetSizer(sizer);
		dialog.CentreOnParent();
		dialog.ShowModal();
	}
	catch (std::exception const& e) { ShowError(context->parent, e); }
}

void UpdateAutomation(agi::Context *context) {
	fs::path root;
	try {
		root = MakeTempRoot();
		auto archive = root / "automation.zip";
		auto stage = root / "package";
		DownloadFile(AUTOMATION_URL, archive, context->parent);
		ExtractZip(archive, stage);
		fs::remove(archive);
		auto destination = config::path->Decode("?user/automation");
		std::string error;
		if (!ApplyPackage(stage, destination, error))
			throw UpdateError("Az Automation fájlok telepítése sikertelen: " + error);
		cmd::call("am/reload/autoload", context);
		wxMessageBox(wxString::Format(_("Muteki Automation scripts were updated successfully.\n\nTarget folder: %s"),
			wxString(destination.wstring())), _("Muteki update"), wxOK | wxICON_INFORMATION, context->parent);
	}
	catch (std::exception const& e) { ShowError(context->parent, e); }
	std::error_code ignored;
	if (!root.empty()) fs::remove_all(root, ignored);
}

} // namespace muteki_update
