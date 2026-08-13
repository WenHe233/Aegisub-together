#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <functional>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <wx/artprov.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/display.h>
#include <wx/listctrl.h>
#include <wx/fontenum.h>
#include <wx/progdlg.h>
#include <wx/srchctrl.h>
#include <wx/stdpaths.h>
#include <wx/time.h>
#include <wx/tokenzr.h>
#include <wx/treectrl.h>
#include <wx/vlbox.h>
#include <wx/wx.h>

#ifdef __WXMSW__
#include <commctrl.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreText/CoreText.h>
#else
#include <fontconfig/fontconfig.h>
#endif

#include "ass_file.h"
#include "ass_dialogue.h"
#include "ass_style.h"
#include "compat.h"
#include "dialog_progress.h"
#include "font_size_object.h"
#include "line_change_flags.h"
#include "options.h"
#include "ui_numeric_slider.cpp"

#include <libaegisub/background_runner.h>
#include <libaegisub/path.h>
#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/io.h>

class DialogFontPicker;

enum class FontCategory {
    All,
    Recent,
    RecentlyInstalled,
    MostUsed,
    Favorites,
    Temporary,
    Custom
};

static FontCategory current_category = FontCategory::All;
static int current_list_index = -1;

struct FontListData {
    wxString name;
    std::vector<wxString> fonts;
};

static bool show_at_fonts = false;

/// One configured language filter. `groups` is the condition in disjunctive form:
/// the font matches when every character of at least one group exists in it, which
/// is what "these characters, or those" needs.
struct LanguageFilter {
    wxString label;
    wxString condition;
    std::vector<std::wstring> groups;
    bool enabled = false;
};

static std::vector<LanguageFilter> g_language_filters;
static bool g_language_filters_loaded = false;

/// Keyed by condition and then by face name, so editing a filter cannot make a
/// stale answer from a different condition apply to it.
static std::unordered_map<std::string, std::unordered_map<std::string, bool>> g_glyph_cache;
static std::unordered_map<std::string, json::Integer> g_usage_cache;
static std::unordered_map<std::string, json::Integer> g_installed_cache;
static std::vector<FontListData> g_custom_lists;
static std::unordered_set<wxString> g_temporary;
static std::unordered_set<wxString> g_favorites;
static std::vector<wxString> g_recent;

static bool g_font_cache_loaded = false;
static bool g_font_cache_dirty = false;

/// "Label|characters/other characters|1": label, the condition, and whether the
/// filter starts switched on. Groups are separated by '/' and are ANDed within.
static LanguageFilter ParseLanguageFilter(std::string const& definition)
{
    LanguageFilter filter;
    wxString value = wxString::FromUTF8(definition);
    wxString label = value.BeforeFirst('|');
    wxString rest = value.AfterFirst('|');
    wxString condition = rest.BeforeFirst('|');
    wxString flag = rest.AfterFirst('|');
    filter.label = label.Trim(true).Trim(false);
    filter.condition = condition.Trim(true).Trim(false);
    filter.enabled = flag.Trim(true).Trim(false) == "1";
    for (auto const& group : wxSplit(filter.condition, '/', 0)) {
        std::wstring characters;
        for (auto ch : group.ToStdWstring())
            if (!std::iswspace(ch)) characters.push_back(ch);
        if (!characters.empty()) filter.groups.push_back(std::move(characters));
    }
    return filter;
}

/// Re-read the configured filters every time the dialog opens, so a filter added
/// in the preferences shows up without a restart. Whether a filter is ticked is
/// remembered for the session and matched up by label and condition; anything new
/// starts from its configured default.
static void LoadLanguageFilters()
{
    std::vector<LanguageFilter> previous = std::move(g_language_filters);
    g_language_filters.clear();
    g_language_filters_loaded = true;
    for (auto const& definition : OPT_GET("Tool/Font Picker/Language Filters")->GetListString()) {
        auto filter = ParseLanguageFilter(definition);
        if (filter.label.empty() || filter.groups.empty()) continue;
        auto found = std::find_if(previous.begin(), previous.end(),
            [&](LanguageFilter const& old_filter) {
                return old_filter.label == filter.label &&
                    old_filter.condition == filter.condition;
            });
        if (found != previous.end()) filter.enabled = found->enabled;
        g_language_filters.push_back(std::move(filter));
    }
}

static json::Integer NowTimestamp()
{
    return (json::Integer)std::time(nullptr);
}

static agi::fs::path GetFontListPath()
{
    return config::path->Decode("?user/font_list.json");
}

static agi::fs::path GetFontCachePath()
{
    return config::path->Decode("?user/font_cache.json");
}

static void LoadFontCache()
{
    if (g_font_cache_loaded)
        return;

    g_font_cache_loaded = true;

    agi::fs::path path = GetFontCachePath();

    try {
        json::UnknownElement root;
        json::Reader::Read(root, *agi::io::Open(path)); 
        json::Object& obj = root;

        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const std::string& name = it->first;

            g_installed_cache[name] = NowTimestamp() - 7 * 24 * 60 * 60;

            try {
                json::Object& entry = it->second;

                // Keyed by the filter condition, so an edited filter is simply a
                // different key rather than a stale answer under an old name.
                if (entry.count("glyphs")) {
                    try {
                        json::Object& glyphs = entry["glyphs"];
                        for (auto glyph = glyphs.begin(); glyph != glyphs.end(); ++glyph) {
                            try {
                                g_glyph_cache[glyph->first][name] = (bool)(json::Boolean&)glyph->second;
                            }
                            catch (...) {}
                        }
                    }
                    catch (...) {}
                }

                if (entry.count("usage")) {
                    try {
                        g_usage_cache[name] = entry["usage"];
                    }
                    catch (...) {}
                }

                if (entry.count("installed_at")) {

                    try {
                        g_installed_cache[name] = entry["installed_at"];
                    }
                    catch (...) {}
                }
            }
            catch (...) {
                continue;
            }

        }
    }
    catch (...) {
    }
}

static void SaveFontCache()
{
    if (!g_font_cache_dirty)
        return;

    agi::fs::path path = GetFontCachePath();
    json::Object root;

    for (auto& [font, timestamp] : g_installed_cache) {
        json::Object entry;

        json::Object glyphs;
        for (auto& [condition, faces] : g_glyph_cache) {
            auto found = faces.find(font);
            if (found != faces.end()) glyphs[condition] = found->second;
        }

        entry["glyphs"] = std::move(glyphs);
        entry["usage"] = g_usage_cache[font];
        entry["installed_at"] = timestamp;

        root[font] = std::move(entry);
    }

    try {
        agi::JsonWriter::Write(root, agi::io::Save(path).Get());
    }
    catch (...) {
    }

    g_font_cache_dirty = false;
}

static void LoadLists()
{
    g_custom_lists.clear();
    g_favorites.clear();
    g_recent.clear();

    agi::fs::path path = GetFontListPath();

    try {
        json::UnknownElement root;
        json::Reader::Read(root, *agi::io::Open(path));
        json::Object& obj = root;

        if (obj.count("lists")) {
            json::Array& lists = obj["lists"];

            for (auto& entry : lists) {
                json::Object& l = entry;

                FontListData data;
                data.name = to_wx(l["name"]);

                if (l.count("fonts")) {
                    json::Array& fonts = l["fonts"];

                    for (auto& f : fonts)
                        data.fonts.push_back(to_wx(f));
                }

                g_custom_lists.push_back(data);
            }
        }

        if (obj.count("favorites")) {
            json::Array& fav = obj["favorites"];

            for (auto& f : fav)
                g_favorites.insert(to_wx(f));
        }

        if (obj.count("recent")) {
            json::Array& rec = obj["recent"];

            for (auto& f : rec)
                g_recent.push_back(to_wx(f));
        }
    }
    catch (...) {
    }
}

static void SaveLists()
{
    agi::fs::path path = GetFontListPath();
    json::Object root;
    json::Array lists;

    for (auto& l : g_custom_lists) {
        json::Object obj;

        obj["name"] = from_wx(l.name);

        json::Array fonts;
        for (auto& f : l.fonts)
            fonts.push_back(from_wx(f));

        obj["fonts"] = std::move(fonts);
        lists.push_back(std::move(obj));
    }

    root["lists"] = std::move(lists);

    json::Array fav;
    for (auto& f : g_favorites)
        fav.push_back(from_wx(f));

    root["favorites"] = std::move(fav);

    json::Array rec;
    for (auto& f : g_recent)
        rec.push_back(from_wx(f));

    root["recent"] = std::move(rec);

    try {
        agi::JsonWriter::Write(root, agi::io::Save(path).Get());
    }
    catch (...) {
    }
}

static void SortCustomLists()
{
    std::sort(g_custom_lists.begin(), g_custom_lists.end(),
        [](const FontListData& a, const FontListData& b) {
            return a.name.CmpNoCase(b.name) < 0;
        });
}

std::set<wxString> CollectFontsFromSubs(AssFile* subs)
{
    std::set<wxString> fonts;
    std::regex re(R"(\\fn([^\\}]+))");

    for (auto& line : subs->Events) {
        std::string text = line.Text.get();

        auto begin = std::sregex_iterator(text.begin(), text.end(), re);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            std::string name = (*it)[1].str();

            wxString wxname = to_wx(name);
            wxname.Trim(true).Trim(false);

            if (!wxname.empty())
                fonts.insert(wxname);
        }

        const AssStyle* style = subs->GetStyle(line.Style);
        if (style && !style->font.empty())
            fonts.insert(to_wx(style->font));
    }

    return fonts;
}

class FontCategorySidebar : public wxPanel {
public:
    wxTreeCtrl* tree;
    wxTreeItemId root;
    wxTreeItemId all;
    wxTreeItemId recent;
    wxTreeItemId recently_installed;
    wxTreeItemId most_used;
    wxTreeItemId favorites;
    wxTreeItemId temporary;
    wxTreeItemId own_lists_root;
    wxButton* create_btn;

    std::function<void(FontCategory)> onCategoryChanged;
    std::function<void(int)> onImportFromSubs;
    std::vector<wxTreeItemId> list_items;

    bool is_rebuilding = false;
    bool suppress_selection_event = false;

    FontCategorySidebar(wxWindow* parent) : wxPanel(parent) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        tree = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_HIDE_ROOT | wxTR_SINGLE | wxBORDER_NONE);
        root = tree->AddRoot("root");
        all        = tree->AppendItem(root, _("All fonts"));
        recently_installed = tree->AppendItem(root, _("Recently installed"));
        recent     = tree->AppendItem(root, _("Recent fonts"));
        most_used  = tree->AppendItem(root, _("Most used"));
        favorites  = tree->AppendItem(root, _("Favorites"));
        temporary  = tree->AppendItem(root, _("Temporary List"));
        own_lists_root = tree->AppendItem(root, _("Own lists") + wxString::FromUTF8(" ↵"));

        tree->Bind(wxEVT_TREE_ITEM_EXPANDED, [this](wxTreeEvent& e){
            if (e.GetItem() == own_lists_root)
                tree->SetItemText(own_lists_root, _("Own lists") + wxString::FromUTF8(" ↴"));
        });

        tree->Bind(wxEVT_TREE_ITEM_COLLAPSED, [this](wxTreeEvent& e){
            if (e.GetItem() == own_lists_root)
                tree->SetItemText(own_lists_root, _("Own lists") + wxString::FromUTF8(" ↵"));
        });

        RebuildLists();

        tree->Expand(own_lists_root);
        tree->SetIndent(12);
        tree->SetWindowStyle(tree->GetWindowStyle() | wxTR_NO_LINES);
        tree->SetBackgroundColour(GetParent()->GetBackgroundColour());
        tree->SetItemBold(own_lists_root, true);

        create_btn = new wxButton(this, wxID_ANY, _("Create new list"));
        create_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxTextEntryDialog dlg(this, "", _("Create new list"));

            if (dlg.ShowModal() == wxID_OK) {
                FontListData l;
                wxString name = dlg.GetValue().Trim().Trim(false);

                if (name.empty()) {
                    wxMessageBox(_("Name cannot be empty"), _("Error"), wxICON_ERROR);
                    return;
                }

                if (ListNameExists(name)) {
                    wxMessageBox(_("List with this name already exists"), _("Error"), wxICON_ERROR);
                    return;
                }

                l.name = name;
                g_custom_lists.push_back(l);

                SaveLists();
                RebuildLists();

                current_category = FontCategory::Custom;
                current_list_index = FindListIndexByName(l.name);
                SelectCurrentItem();
            }
        });

        sizer->Add(create_btn, 0, wxEXPAND | wxALL, 4);
        sizer->Add(tree, 1, wxEXPAND | wxALL, 4);
        SetSizer(sizer);

        tree->Bind(wxEVT_TREE_SEL_CHANGED, &FontCategorySidebar::OnSelChanged, this);
        tree->Bind(wxEVT_TREE_ITEM_RIGHT_CLICK, &FontCategorySidebar::OnRightClick, this);

        if (current_category == FontCategory::Temporary)
            current_category = FontCategory::All;
    }

    void RebuildLists()
    {
        is_rebuilding = true;
        suppress_selection_event = true;

        SortCustomLists();

        tree->Freeze();
        tree->DeleteChildren(own_lists_root);

        list_items.clear();

        for (size_t i = 0; i < g_custom_lists.size(); ++i) {
            auto item = tree->AppendItem(
                own_lists_root,
                g_custom_lists[i].name
            );

            list_items.push_back(item);
        }

        tree->Expand(own_lists_root);
        tree->Thaw();

        is_rebuilding = false;
        CallAfter([this]{
            suppress_selection_event = false;
        });
    }

    void OnRightClick(wxTreeEvent& evt)
    {
        auto item = evt.GetItem();
        wxString name = tree->GetItemText(item);

        if (name.StartsWith("> "))
            name = name.Mid(2);

        int idx = FindListIndexByName(name);

        tree->SelectItem(item);
        UpdateActiveVisual(item);

        if (idx < 0)
            return;

        wxMenu menu;
        menu.Append(1, _("Rename"));
        menu.Append(2, _("Duplicate"));
        menu.Append(3, _("Delete"));
        menu.Append(4, _("Import from the subtitle"));

        int res = GetPopupMenuSelectionFromUser(menu);

        if (res == 1) {
            RenameList(idx);
        } else if (res == 2) {
            DuplicateList(idx);
        } else if (res == 3) {
            DeleteList(idx);
        } else if (res == 4) {
            if (onImportFromSubs)
                onImportFromSubs(idx);
        }
    }

    int FindListIndexByName(const wxString& name)
    {
        for (size_t i = 0; i < g_custom_lists.size(); ++i) {
            if (g_custom_lists[i].name == name)
                return (int)i;
        }

        return -1;
    }

    bool ListNameExists(const wxString& name, const wxString& ignore = "")
    {
        for (const auto& l : g_custom_lists) {
            if (l.name.CmpNoCase(name) == 0 && l.name.CmpNoCase(ignore) != 0)
                return true;
        }

        return false;
    }

    void RenameList(int idx)
    {
        wxString oldName = g_custom_lists[idx].name;
        wxTextEntryDialog dlg(this, "", _("Rename"), oldName);

        if (dlg.ShowModal() == wxID_OK) {
            wxString newName = dlg.GetValue().Trim().Trim(false);

            if (newName.empty()) {
                wxMessageBox(_("Name cannot be empty"), _("Error"), wxICON_ERROR);
                return;
            }

            if (ListNameExists(newName, oldName)) {
                wxMessageBox(_("List with this name already exists"), _("Error"), wxICON_ERROR);
                return;
            }

            g_custom_lists[idx].name = newName;

            SaveLists();
            RebuildLists();

            current_category = FontCategory::Custom;
            current_list_index = FindListIndexByName(newName);
            SelectCurrentItem();
        }
    }

    void DuplicateList(int idx)
    {
        FontListData copy = g_custom_lists[idx];
        copy.name += " (2)";
        g_custom_lists.push_back(copy);

        SaveLists();
        RebuildLists();

        current_category = FontCategory::Custom;
        current_list_index = FindListIndexByName(copy.name);
        SelectCurrentItem();
    }

    void DeleteList(int idx)
    {
        g_custom_lists.erase(g_custom_lists.begin() + idx);

        current_category = FontCategory::All;
        current_list_index = -1;

        SaveLists();
        RebuildLists();
        SelectCurrentItem();
    }

    void SelectCurrentItem()
    {
        wxTreeItemId item;

        switch (current_category) {
            case FontCategory::All:
                item = all;
                break;
            case FontCategory::RecentlyInstalled:
                item = recently_installed;
                break;
            case FontCategory::Recent:
                item = recent;
                break;
            case FontCategory::MostUsed:
                item = most_used;
                break;
            case FontCategory::Favorites:
                item = favorites;
                break;
            case FontCategory::Temporary:
                item = temporary;
                break;
            case FontCategory::Custom:
                if (current_list_index >= 0 && current_list_index < (int)list_items.size())
                    item = list_items[current_list_index];
                break;
        }

        if (item.IsOk()) {
            tree->SelectItem(item);
            UpdateActiveVisual(item);

            CallAfter([this]{
                tree->UnselectAll();
            });

            if (onCategoryChanged)
                onCategoryChanged(current_category);
        }
    }

    void UpdateActiveVisual(wxTreeItemId active)
    {
        tree->SetItemBold(all, false);
        tree->SetItemBold(recently_installed, false);
        tree->SetItemBold(recent, false);
        tree->SetItemBold(most_used, false);
        tree->SetItemBold(favorites, false);
        tree->SetItemBold(temporary, false);

        for (auto& it : list_items)
            tree->SetItemBold(it, false);

        auto resetText = [&](wxTreeItemId item, const wxString& base) {
            tree->SetItemText(item, base);
        };

        resetText(all, _("All fonts"));
        resetText(recently_installed, _("Recently installed"));
        resetText(recent, _("Recent fonts"));
        resetText(most_used, _("Most used"));
        resetText(favorites, _("Favorites"));
        resetText(temporary, _("Temporary List"));

        for (size_t i = 0; i < list_items.size(); ++i) {
            tree->SetItemText(list_items[i], g_custom_lists[i].name);
        }

        if (active.IsOk() && active != own_lists_root) {
            tree->SetItemBold(active, true);

            wxString text = tree->GetItemText(active);

            if (!text.StartsWith("> "))
                tree->SetItemText(active, "> " + text);
        }
    }
private:
    void OnSelChanged(wxTreeEvent& evt)
    {
        if (!onCategoryChanged || is_rebuilding || suppress_selection_event)
            return;

        auto item = evt.GetItem();

        UpdateActiveVisual(item);

        if (item == all) {
            current_list_index = -1;
            current_category = FontCategory::All;
        } else if (item == recently_installed) {
            current_list_index = -1;
            current_category = FontCategory::RecentlyInstalled;
        } else if (item == recent) {
            current_list_index = -1;
            current_category = FontCategory::Recent;
        } else if (item == most_used) {
            current_list_index = -1;
            current_category = FontCategory::MostUsed;
        } else if (item == favorites) {
            current_list_index = -1;
            current_category = FontCategory::Favorites;
        } else if (item == temporary) {
            current_list_index = -1;
            current_category = FontCategory::Temporary;
        } else {
            for (size_t i = 0; i < list_items.size(); ++i) {
                if (list_items[i] == item) {
                    current_list_index = (int)i;
                    current_category = FontCategory::Custom;

                    break;
                }
            }
        }

        onCategoryChanged(current_category);
    }
};

// --------------------------------------------------------------- font previews

/// One byte per pixel saying how much of a glyph covers it, rather than a finished
/// picture: a row can then be painted in whatever colours the theme and the
/// selection ask for, and one file serves a light and a dark scheme alike.
struct FontPreview {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> coverage;

    bool ok() const { return width > 0 && height > 0 && !coverage.empty(); }
};

/// The size the coverage is rasterised at, which is the largest the sample size
/// slider offers. Every size the slider can ask for is then a reduction, and
/// reducing text looks right where enlarging it does not - so one render serves
/// the whole range and the expensive pass never has to run a second time.
/// Measured against rendering at the size in use: 22.4 s instead of 18.4 s to
/// build, and 39 MB of coverage instead of 5.5 MB. Keep in step with the slider
/// in BuildUI.
static const int kPreviewMasterSize = 30;

/// The picker's sample text drawn once per installed face and kept on disk.
///
/// The reason this exists at all: a face's *first* render is expensive and cannot
/// be hidden. Measured over the 1790 faces installed on one machine, rendering the
/// sample once each came to 18 s in total - median 2 ms, but the worst single face
/// 1.35 s. No time budget helps, because the cost cannot be interrupted part way
/// through one face, so a list that renders while it scrolls will stutter however
/// cleverly the work is scheduled. Paying it once, up front, under the progress bar
/// that the glyph probing already shows, is the only arrangement that does not.
///
/// Written with wxWidgets drawing rather than the win32 the rest of this dialog
/// uses, because this is the part that has to work on the Mac later.
class FontPreviewStore {
    std::unordered_map<wxString, FontPreview> previews;
    agi::fs::path path;
    wxString sample;
    int point_size = 0;
    int scale_percent = 0;
    bool dirty = false;

    static const uint32_t kMagic = 0x50465341;   // 'ASFP'
    static const uint32_t kVersion = 1;

    static uint32_t Hash(wxString const& text)
    {
        uint32_t hash = 2166136261u;
        for (unsigned char byte : text.ToStdString(wxConvUTF8)) {
            hash ^= byte;
            hash *= 16777619u;
        }
        return hash;
    }

    static void WriteU32(std::ostream& stream, uint32_t value)
    {
        // Written a byte at a time so the file does not depend on the byte order
        // or the padding of whatever machine wrote it.
        for (int shift = 0; shift < 32; shift += 8)
            stream.put((char)((value >> shift) & 0xFF));
    }

    static bool ReadU32(std::istream& stream, uint32_t& value)
    {
        value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            int byte = stream.get();
            if (byte == std::istream::traits_type::eof()) return false;
            value |= ((uint32_t)(byte & 0xFF)) << shift;
        }
        return true;
    }

public:
    /// Point the store at one sample text and display scale, and read back what was
    /// rendered for it before. The size is not part of the choice: everything is
    /// rasterised at kPreviewMasterSize and reduced when drawn, so changing the
    /// sample size never invalidates anything. Only the sample text or the display
    /// scale can, and both are a different file, so going back to one used before
    /// costs nothing.
    void Use(wxString const& sample_text, double content_scale)
    {
        int percent = std::max(50, std::min(400, (int)std::lround(content_scale * 100.0)));
        if (sample == sample_text && scale_percent == percent && !path.empty())
            return;

        Save();

        previews.clear();
        dirty = false;
        sample = sample_text;
        point_size = kPreviewMasterSize;
        scale_percent = percent;

        path = config::path->Decode(
            wxString::Format("?user/font_previews-%d-%d-%08x.bin",
                point_size, scale_percent, Hash(sample)).utf8_string());

        Load();
    }

    bool Has(wxString const& face) const { return previews.count(face) > 0; }

    /// The size the coverage was rasterised at. A row wanting a different size
    /// scales what is here rather than going without a preview.
    int RenderedSize() const { return point_size; }

    /// Throw every rendered sample away so the next pass renders them all again.
    /// Marked dirty so the emptied state is written out even if nothing follows.
    void Forget()
    {
        previews.clear();
        dirty = true;
    }

    FontPreview const* Find(wxString const& face) const
    {
        auto found = previews.find(face);
        return found == previews.end() ? nullptr : &found->second;
    }

    /// Draw the sample once and keep the coverage. A face that will not realise is
    /// stored as an empty entry, so it is not attempted again on every open.
    void Render(wxString const& face, wxFont const& base)
    {
        FontPreview preview;
        wxFont font = base;
        font.SetPointSize(point_size);

        if (font.SetFaceName(face) && font.IsOk()) {
            wxBitmap probe(1, 1, 24);
            wxMemoryDC measure(probe);
            measure.SetFont(font);
            wxSize extent = measure.GetTextExtent(sample);
            measure.SelectObject(wxNullBitmap);

            // Generous caps rather than tight ones: measured over 1790 faces at the
            // master size the widest sample came to 907 px and the tallest line to
            // 224 px, so these only ever catch something pathological.
            int width = std::min(std::max(extent.GetWidth(), 1), 2000);
            int height = std::min(std::max(extent.GetHeight(), 1), 400);

            wxBitmap canvas;
            if (canvas.CreateWithLogicalSize(wxSize(width, height), scale_percent / 100.0, 24)) {
                wxMemoryDC draw(canvas);
                draw.SetBackground(*wxWHITE_BRUSH);
                draw.Clear();
                draw.SetFont(font);
                draw.SetTextForeground(*wxBLACK);
                draw.SetTextBackground(*wxWHITE);
                draw.DrawText(sample, 0, 0);
                draw.SelectObject(wxNullBitmap);

                wxImage image = canvas.ConvertToImage();
                if (image.IsOk()) {
                    preview.width = image.GetWidth();
                    preview.height = image.GetHeight();
                    preview.coverage.resize((size_t)preview.width * preview.height);

                    // Black on white, so coverage is what the ink took away. The
                    // three channels are averaged because subpixel antialiasing
                    // makes them differ, and a tinted row wants one value.
                    unsigned char const* rgb = image.GetData();
                    for (size_t i = 0; i < preview.coverage.size(); ++i) {
                        int sum = rgb[i * 3] + rgb[i * 3 + 1] + rgb[i * 3 + 2];
                        preview.coverage[i] = (unsigned char)(255 - sum / 3);
                    }
                }
            }
        }

        previews[face] = std::move(preview);
        dirty = true;
    }

    void Save()
    {
        if (!dirty || path.empty()) return;
        dirty = false;

        try {
            agi::io::Save writer(path, true);
            std::ostream& stream = writer.Get();

            WriteU32(stream, kMagic);
            WriteU32(stream, kVersion);
            WriteU32(stream, (uint32_t)point_size);
            WriteU32(stream, (uint32_t)scale_percent);
            WriteU32(stream, (uint32_t)previews.size());

            for (auto const& [face, preview] : previews) {
                std::string name = face.ToStdString(wxConvUTF8);
                WriteU32(stream, (uint32_t)name.size());
                stream.write(name.data(), (std::streamsize)name.size());
                WriteU32(stream, (uint32_t)preview.width);
                WriteU32(stream, (uint32_t)preview.height);
                if (!preview.coverage.empty())
                    stream.write((char const*)preview.coverage.data(),
                        (std::streamsize)preview.coverage.size());
            }
        }
        catch (...) {
        }
    }

private:
    void Load()
    {
        try {
            auto stream_holder = agi::io::Open(path, true);
            std::istream& stream = *stream_holder;

            uint32_t magic = 0, version = 0, size = 0, scale = 0, count = 0;
            if (!ReadU32(stream, magic) || magic != kMagic) return;
            if (!ReadU32(stream, version) || version != kVersion) return;
            if (!ReadU32(stream, size) || (int)size != point_size) return;
            if (!ReadU32(stream, scale) || (int)scale != scale_percent) return;
            if (!ReadU32(stream, count)) return;

            for (uint32_t i = 0; i < count; ++i) {
                uint32_t name_length = 0, width = 0, height = 0;
                if (!ReadU32(stream, name_length) || name_length > 1024) return;

                std::string name(name_length, '\0');
                if (name_length && !stream.read(&name[0], (std::streamsize)name_length)) return;
                if (!ReadU32(stream, width) || !ReadU32(stream, height)) return;
                if (width > 2000 || height > 400) return;

                FontPreview preview;
                preview.width = (int)width;
                preview.height = (int)height;
                size_t bytes = (size_t)width * height;
                if (bytes) {
                    preview.coverage.resize(bytes);
                    if (!stream.read((char*)preview.coverage.data(), (std::streamsize)bytes))
                        return;
                }
                previews[wxString::FromUTF8(name)] = std::move(preview);
            }
        }
        catch (...) {
        }
    }
};

static FontPreviewStore g_preview_store;

class FontListModel {
public:
    std::vector<wxString> fonts;
    std::vector<wxString> filtered;
    std::unordered_map<wxString, std::vector<wxString>> style_cache;
    std::unordered_map<wxString, wxString> lower_cache;
    std::unordered_set<wxString> font_set;
    wxString initial_face;

    void LoadFonts() {
        fonts.clear();

        wxFontEnumerator fe;
        wxArrayString faces = fe.GetFacenames();

        for (auto const& f : faces) {
            fonts.push_back(f);
        }

        std::sort(fonts.begin(), fonts.end(),
            [](const wxString& a, const wxString& b) {
                return a.CmpNoCase(b) < 0;
            });

        WarmCaches();

        lower_cache.clear();
        for (auto const& f : fonts)
            lower_cache[f] = f.Lower();

        font_set.clear();
        for (auto const& f : fonts)
            font_set.insert(f);

        json::Integer now = NowTimestamp();
        for (auto const& f : fonts) {
            std::string key = f.ToStdString();

            if (!g_installed_cache.count(key)) {
                g_installed_cache[key] = now;
                g_font_cache_dirty = true;
            }
        }

        filtered = fonts;
    }

    /// Ask every filter about every face, so the list can be filtered without a
    /// stall later. Each answer costs a GDI probe, so the first run over a few
    /// hundred faces is slow enough to need telling the user about.
    ///
    /// Whatever this measures is written out before returning. The answers are
    /// only invalidated by the font set changing or by the refresh button, so
    /// paying for them once per installed font is the point; leaving them until
    /// the dialog is confirmed meant a cancelled dialog threw the whole run away.
    /// Both are the same kind of debt - a per-face cost that has to be paid once -
    /// so they share one wait and one bar. Everything measured is written out
    /// before returning, including after a cancel: the faces done up to that point
    /// are just as valid, and keeping them makes the next open shorter.
    ///
    /// Deliberately on the calling thread. The glyph probe would survive a worker,
    /// but drawing a preview goes through wxWidgets, and wxBitmap and wxMemoryDC
    /// belong to the main thread - on the Mac that is not a nicety.
    void WarmCaches()
    {
        wxFont base = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        double content_scale = 1.0;
        if (wxWindow* top = wxTheApp ? wxTheApp->GetTopWindow() : nullptr)
            content_scale = top->GetContentScaleFactor();

        g_preview_store.Use(
            wxString::FromUTF8(OPT_GET("Tool/Font Picker/Sample")->GetString()),
            content_scale);

        std::vector<wxString> glyph_pending;
        for (auto const& f : fonts) {
            for (auto const& filter : g_language_filters) {
                auto const& cache = g_glyph_cache[filter.condition.ToStdString(wxConvUTF8)];
                if (!cache.count(f.ToStdString())) { glyph_pending.push_back(f); break; }
            }
        }

        std::vector<wxString> preview_pending;
        for (auto const& f : fonts)
            if (!g_preview_store.Has(f)) preview_pending.push_back(f);

        size_t total = glyph_pending.size() + preview_pending.size();
        if (!total) return;

        // Only worth a dialog when there is a real wait; a handful of new fonts is
        // faster to just do.
        if (total < 25) {
            for (auto const& f : glyph_pending)
                for (auto const& filter : g_language_filters)
                    FontMatchesFilter(filter, f);
            for (auto const& f : preview_pending)
                g_preview_store.Render(f, base);
            SaveFontCache();
            g_preview_store.Save();
            return;
        }

        wxProgressDialog progress(_("Font Picker"), _("Caching fonts..."), (int)total, nullptr,
            wxPD_CAN_ABORT | wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);

        int done = 0;
        bool cancelled = false;

        for (auto const& f : glyph_pending) {
            for (auto const& filter : g_language_filters)
                FontMatchesFilter(filter, f);
            if (!progress.Update(++done)) { cancelled = true; break; }
        }

        if (!cancelled) {
            for (auto const& f : preview_pending) {
                g_preview_store.Render(f, base);
                if (!progress.Update(++done)) break;
            }
        }

        SaveFontCache();
        g_preview_store.Save();
    }

    void CleanInvalidFonts()
    {
        for (auto& list : g_custom_lists) {
            list.fonts.erase(
                std::remove_if(list.fonts.begin(), list.fonts.end(),
                    [&](const wxString& f) { return !FontExists(f); }),
                list.fonts.end()
            );
        }

        for (auto it = g_favorites.begin(); it != g_favorites.end(); ) {
            if (!FontExists(*it))
                it = g_favorites.erase(it);
            else
                ++it;
        }

        g_recent.erase(
            std::remove_if(g_recent.begin(), g_recent.end(),
                [&](const wxString& f) { return !FontExists(f); }),
            g_recent.end()
        );
    }

    void Filter(wxString const& text)
    {
        filtered.clear();
        wxString t = text.Lower();

        if (current_category == FontCategory::RecentlyInstalled) {
            json::Integer now = NowTimestamp();
            int filter_days = 3 * 24 * 60 * 60;

            for (auto const& f : fonts) {
                if (f.StartsWith("@"))
                    continue;

                std::string key = f.ToStdString();

                auto it = g_installed_cache.find(key);
                if (it == g_installed_cache.end())
                    continue;

                if (now - it->second > filter_days)
                    continue;

                if (t.empty() || lower_cache[f].Contains(t))
                    filtered.push_back(f);
            }

            std::sort(filtered.begin(), filtered.end(),
                [](const wxString& a, const wxString& b) {
                    return g_installed_cache[a.ToStdString()] >
                        g_installed_cache[b.ToStdString()];
                });

            return;
        }

        if (current_category == FontCategory::Recent) {
            for (auto const& f : g_recent) {
                if (!FontExists(f))
                    continue;

                if (t.empty() || lower_cache[f].Contains(t))
                    filtered.push_back(f);
            }

            return;
        }

        if (current_category == FontCategory::Favorites) {
            for (auto const& f : g_favorites) {
                if (!FontExists(f))
                    continue;

                if (t.empty() || lower_cache[f].Contains(t))
                    filtered.push_back(f);
            }

            std::sort(filtered.begin(), filtered.end(),
                [](const wxString& a, const wxString& b) {
                    return a.CmpNoCase(b) < 0;
                });

            return;
        }

        if (current_category == FontCategory::Temporary) {
            for (auto const& f : fonts) {
                if (!g_temporary.count(f))
                    continue;

                if (t.empty() || lower_cache[f].Contains(t))
                    filtered.push_back(f);
            }

            return;
        }

        if (current_category == FontCategory::MostUsed) {
            for (auto const& f : fonts) {
                auto it = g_usage_cache.find(f.ToStdString());
                if (it != g_usage_cache.end() && it->second > 0)
                    filtered.push_back(f);
            }

            std::sort(filtered.begin(), filtered.end(),
                [](const wxString& a, const wxString& b) {
                    return g_usage_cache[a.ToStdString()] >
                        g_usage_cache[b.ToStdString()];
                });

            if (filtered.size() > 20)
                filtered.resize(20);

            if (!t.empty()) {
                std::vector<wxString> tmp;

                for (auto const& f : filtered) {
                    if (lower_cache[f].Contains(t))
                        tmp.push_back(f);
                }

                filtered = std::move(tmp);
            }

            return;
        }

        if (current_category == FontCategory::Custom) {
            if (current_list_index >= 0 && current_list_index < (int)g_custom_lists.size()) {
                std::vector<wxString> sorted = g_custom_lists[current_list_index].fonts;

                std::sort(sorted.begin(), sorted.end(),
                    [](const wxString& a, const wxString& b) {
                        return a.CmpNoCase(b) < 0;
                    });

                for (auto const& f : sorted) {
                    if (!FontExists(f))
                        continue;

                    if (t.empty() || lower_cache[f].Contains(t))
                        filtered.push_back(f);
                }
            }

            return;
        }

        for (auto const& f : fonts) {
            if (f == initial_face) {
                if (t.empty() || lower_cache[f].Contains(t))
                    filtered.push_back(f);
                continue;
            }

            if (!show_at_fonts && f.StartsWith("@"))
                continue;

            bool rejected = false;
            for (auto const& filter : g_language_filters) {
                if (filter.enabled && !FontMatchesFilter(filter, f)) {
                    rejected = true;
                    break;
                }
            }
            if (rejected)
                continue;

            if (t.empty() || lower_cache[f].Contains(t))
                filtered.push_back(f);
        }
    }

    bool FontExists(const wxString& name) const
    {
        return font_set.count(name) > 0;
    }

    /// True when the face satisfies the filter: every character of at least one
    /// of its groups has a glyph. Answers are cached per condition, since probing
    /// a face costs a device context and a glyph lookup.
    bool FontMatchesFilter(LanguageFilter const& filter, wxString const& face)
    {
        if (filter.groups.empty()) return true;
        std::string condition = filter.condition.ToStdString(wxConvUTF8);
        auto& cache = g_glyph_cache[condition];
        std::string key = face.ToStdString();
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;

#ifdef __WXMSW__
        HDC hdc = GetDC(nullptr);
        LOGFONTW lf{};
        wcsncpy_s(lf.lfFaceName, face.wc_str(), LF_FACESIZE - 1);
        HFONT font = CreateFontIndirectW(&lf);
        HFONT old_font = font ? (HFONT)SelectObject(hdc, font) : nullptr;

        auto has_char = [&](wchar_t ch) {
            if (!hdc || !font) return false;
            WORD glyph = 0xFFFF;
            GetGlyphIndicesW(hdc, &ch, 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS);
            return glyph != 0xFFFF;
        };
#elif defined(__APPLE__)
        auto face_utf8 = face.utf8_str();
        CFStringRef face_name = CFStringCreateWithCString(kCFAllocatorDefault,
            face_utf8.data(), kCFStringEncodingUTF8);
        CTFontRef font = face_name ? CTFontCreateWithName(face_name, 12.0, nullptr) : nullptr;
        if (face_name) CFRelease(face_name);

        auto has_char = [&](wchar_t ch) {
            if (!font) return false;
            wxString text(wxUniChar(ch));
            auto utf8 = text.utf8_str();
            CFStringRef value = CFStringCreateWithCString(kCFAllocatorDefault,
                utf8.data(), kCFStringEncodingUTF8);
            if (!value) return false;
            CFIndex length = CFStringGetLength(value);
            std::vector<UniChar> characters(static_cast<size_t>(length));
            std::vector<CGGlyph> glyphs(static_cast<size_t>(length));
            CFStringGetCharacters(value, CFRangeMake(0, length), characters.data());
            bool present = CTFontGetGlyphsForCharacters(font, characters.data(),
                glyphs.data(), length);
            CFRelease(value);
            return present;
        };
#else
        auto face_utf8 = face.utf8_str();
        FcPattern *request = FcPatternCreate();
        if (request)
            FcPatternAddString(request, FC_FAMILY,
                reinterpret_cast<FcChar8 const *>(face_utf8.data()));
        if (request) {
            FcConfigSubstitute(nullptr, request, FcMatchPattern);
            FcDefaultSubstitute(request);
        }
        FcResult result = FcResultNoMatch;
        FcPattern *font = request ? FcFontMatch(nullptr, request, &result) : nullptr;
        FcCharSet *charset = nullptr;
        if (font)
            FcPatternGetCharSet(font, FC_CHARSET, 0, &charset);

        auto has_char = [&](wchar_t ch) {
            return charset && FcCharSetHasChar(charset, static_cast<FcChar32>(ch));
        };
#endif

        bool ok = false;
        for (auto const& group : filter.groups) {
            bool all = true;
            for (auto ch : group)
                if (!has_char(ch)) { all = false; break; }
            if (all) { ok = true; break; }
        }

#ifdef __WXMSW__
        if (old_font) SelectObject(hdc, old_font);
        if (font) DeleteObject(font);
        if (hdc) ReleaseDC(nullptr, hdc);
#elif defined(__APPLE__)
        if (font) CFRelease(font);
#else
        if (font) FcPatternDestroy(font);
        if (request) FcPatternDestroy(request);
#endif

        cache[key] = ok;
        g_font_cache_dirty = true;

        return ok;
    }
};

/// Everything about a font row that does not depend on which control draws it:
/// the sample, the tinted bitmaps and the context menu. Both list controls below
/// own one of these, so the only thing that differs per platform is the painting.
class FontRowPainter {
    FontListModel* model;
    wxString sampleText;
    int sampleSize;
    wxFont baseFont;
    int row_height = 0;

    /// The coverage as a bitmap in the text colour with the coverage as its alpha,
    /// ready to compose over whatever is behind it. Alpha rather than a finished
    /// picture with a background baked in, so the row keeps the selection the
    /// platform itself drew instead of one guessed at here.
    ///
    /// Derived from the store and cheap to rebuild, so it can be dropped whenever;
    /// the expensive part is the coverage, which the store holds.
    mutable std::unordered_map<wxString, wxBitmap> tinted;
    /// Where a selected row is tinted. There is only ever one, so it does not need
    /// a place in the cache.
    mutable wxBitmap scratch;

public:
    std::function<void()> onFilterChanged;
    std::function<void()> onRepaint;

    explicit FontRowPainter(FontListModel* model) : model(model)
    {
        baseFont = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        sampleSize = OPT_GET("Tool/Font Picker/Sample Size")->GetInt();
        sampleText = wxString::FromUTF8(OPT_GET("Tool/Font Picker/Sample")->GetString());
        Measure();
    }

    int RowHeight() const { return row_height; }

    /// The generic face at the sample size. Only the row height comes from this:
    /// the sample size is how big the preview should be, and letting the name
    /// column follow it made the names grow along with the samples.
    wxFont RowFont() const
    {
        wxFont font = baseFont;
        font.SetPointSize(sampleSize);
        return font;
    }

    /// What the face name is written in, at whatever size the interface uses. Fixed,
    /// so the preview slider moves the preview and nothing else.
    wxFont NameFont() const { return baseFont; }

    void SetSampleSize(int size)
    {
        sampleSize = size;
        // Nothing to rebuild: the store holds the sample at the largest size the
        // slider offers, so every position is a reduction of what is already in
        // memory. That is why the slider is instant and why the expensive pass
        // never runs again once it has run.
        tinted.clear();
        Measure();
    }

    void InvalidateTints() { tinted.clear(); }

    /// A favourite is marked in the name itself, so a control that lets the platform
    /// draw that column gets the star without having to know about favourites.
    wxString RowName(wxString const& face) const
    {
        if (g_favorites.count(face))
            return face + wxString::FromUTF8(" \xe2\x98\x85");
        return face;
    }

    static bool IsFavorite(wxString const& face) { return g_favorites.count(face) > 0; }
    static wxColour FavoriteColour() { return wxColour(200, 170, 0); }

    /// The sample for one face in `ink`, at the size the slider asks for, or nullptr
    /// when the store has nothing rendered for it. The returned bitmap is owned by
    /// the painter and stays valid until the tints are dropped.
    ///
    /// `cacheable` is false for the colour a selected row wants: there is only ever
    /// one such row, and keeping a second bitmap per face for it would double the
    /// cache to no purpose.
    wxBitmap const* PreviewFor(wxString const& face, wxColour const& ink,
                               bool cacheable = true) const
    {
        FontPreview const* preview = g_preview_store.Find(face);
        if (!preview || !preview->ok())
            return nullptr;

        // Reduced from the master size to whatever the slider asks for. Always a
        // reduction, never an enlargement, which is the whole reason the store
        // renders at the top of the range.
        int rendered = g_preview_store.RenderedSize();
        double factor = (rendered > 0 && rendered != sampleSize)
            ? (double)sampleSize / rendered : 1.0;

        int width = std::max(1, (int)std::lround(preview->width * factor));
        int height = std::max(1, (int)std::lround(preview->height * factor));

        if (!cacheable) {
            scratch = Tint(*preview, width, height, ink);
            return &scratch;
        }

        auto found = tinted.find(face);
        if (found == tinted.end() ||
            found->second.GetWidth() != width || found->second.GetHeight() != height) {
            // Scrolling a few thousand faces would otherwise hold a few thousand
            // bitmaps. Dropping the lot is safe: rebuilding one is a tint of bytes
            // already in memory, with no font work anywhere in it.
            if (tinted.size() > 600) tinted.clear();
            found = tinted.insert_or_assign(face, Tint(*preview, width, height, ink)).first;
        }
        return &found->second;
    }

    /// Draw the preview through a wxDC, over whatever the row's background already
    /// is. Used where the control paints its own rows.
    void DrawPreview(wxDC& dc, wxRect const& cell, wxString const& face,
                     wxColour const& ink, bool cacheable) const
    {
        if (cell.width <= 0 || cell.height <= 0)
            return;

        wxBitmap const* bitmap = PreviewFor(face, ink, cacheable);
        if (!bitmap)
            return;

        wxDCClipper clip(dc, cell);
        dc.DrawBitmap(*bitmap, cell.x,
            cell.y + (cell.height - bitmap->GetHeight()) / 2, true);
    }

    // ------------------------------------------------------------ context menu

    void ShowContextMenu(wxWindow* owner, wxString const& font)
    {
        wxMenu menu;

        if (g_temporary.count(font))
            menu.Append(1, _("Remove from Temporary List"));
        else
            menu.Append(1, _("Add to Temporary List"));

        menu.Append(2, _("Copy font's name"));

        if (g_favorites.count(font))
            menu.Append(3, _("Remove from Favorites"));
        else
            menu.Append(3, _("Add to Favorites"));

        wxMenu* assignMenu = new wxMenu();

        for (size_t i = 0; i < g_custom_lists.size(); ++i) {
            assignMenu->Append(1000 + i, g_custom_lists[i].name);
        }

        if (!g_custom_lists.empty())
            menu.AppendSubMenu(assignMenu, _("Assign to list"));

        if (current_category == FontCategory::Custom && current_list_index >= 0 && current_list_index < (int)g_custom_lists.size()) {
            auto& list = g_custom_lists[current_list_index];

            if (std::find(list.fonts.begin(), list.fonts.end(), font) != list.fonts.end()) {
                menu.Append(4, _("Remove from current list"));
            }
        }

        int res = owner->GetPopupMenuSelectionFromUser(menu);
        if (res == 1) {
            ToggleTemporary(font);
        } else if (res == 2) {
            CopyToClipboard(font);
        } else if (res == 3) {
            ToggleFavorite(font);
        } else if (res == 4) {
            RemoveFromCurrentList(font);
        } else if (res >= 1000) {
            AssignToList(font, res - 1000);
        }
    }

    void ToggleTemporary(const wxString& font)
    {
        if (g_temporary.count(font))
            g_temporary.erase(font);
        else
            g_temporary.insert(font);

        if (current_category == FontCategory::Temporary && onFilterChanged)
            onFilterChanged();
    }

    void CopyToClipboard(const wxString& font)
    {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(font));
            wxTheClipboard->Close();
            wxTheClipboard->Flush();
        }
    }

    void ToggleFavorite(const wxString& font)
    {
        if (g_favorites.count(font))
            g_favorites.erase(font);
        else
            g_favorites.insert(font);

        SaveLists();

        tinted.clear();
        if (onRepaint) onRepaint();

        if (current_category == FontCategory::Favorites && onFilterChanged)
            onFilterChanged();
    }

    void AssignToList(const wxString& font, int idx)
    {
        if (idx < 0 || idx >= (int)g_custom_lists.size())
            return;

        auto& list = g_custom_lists[idx];

        if (std::find(list.fonts.begin(), list.fonts.end(), font) == list.fonts.end())
            list.fonts.push_back(font);

        SaveLists();
    }

    void RemoveFromCurrentList(const wxString& font)
    {
        if (current_category != FontCategory::Custom || current_list_index < 0 || current_list_index >= (int)g_custom_lists.size())
            return;

        auto& list = g_custom_lists[current_list_index];

        list.fonts.erase(
            std::remove(list.fonts.begin(), list.fonts.end(), font),
            list.fonts.end()
        );

        SaveLists();

        if (onFilterChanged)
            onFilterChanged();
    }

private:
    /// A row is as tall as the sample needs at the current size, so the height
    /// follows the slider. Measured from the generic face: the store's coverage is
    /// never taller than the line the same size asked for.
    void Measure()
    {
        wxBitmap probe(1, 1, 24);
        wxMemoryDC dc(probe);
        dc.SetFont(RowFont());
        row_height = std::max(dc.GetCharHeight() + 4, 8);
    }

    /// Coverage becomes the alpha channel and `ink` the colour, so the result can be
    /// composed over anything: the row's ordinary background, the platform's own
    /// selection, light scheme or dark. That is the whole reason the store keeps
    /// coverage rather than a finished picture.
    wxBitmap Tint(FontPreview const& preview, int width, int height,
                  wxColour const& ink) const
    {
        wxImage image(preview.width, preview.height);
        image.InitAlpha();

        unsigned char* rgb = image.GetData();
        unsigned char* alpha = image.GetAlpha();
        size_t pixels = (size_t)preview.width * preview.height;

        for (size_t i = 0; i < pixels; ++i) {
            rgb[i * 3 + 0] = ink.Red();
            rgb[i * 3 + 1] = ink.Green();
            rgb[i * 3 + 2] = ink.Blue();
            alpha[i] = preview.coverage[i];
        }

        if (width != preview.width || height != preview.height)
            image.Rescale(width, height, wxIMAGE_QUALITY_BILINEAR);

        return wxBitmap(image);
    }
};

#ifdef __WXMSW__

/// The native list, kept on Windows so the list behaves and looks the way it always
/// has here: real columns, native selection and scrolling.
///
/// The preview column is the one thing drawn by hand, because it is a bitmap now
/// rather than text. NM_CUSTOMDRAW is the hook for that, and it exists only on
/// Windows - which is why the other platforms get the wxVListBox below instead of
/// this. The tinting itself is shared; only the blit is win32.
class FontListCtrl : public wxListCtrl {
    FontListModel* model;
    FontRowPainter painter;
    mutable std::unordered_map<wxString, std::unique_ptr<wxItemAttr>> attr_cache_name;

public:
    std::function<void()> onFilterChanged;
    std::function<void()> onSelectionChanged;
    std::function<void()> onActivated;

    FontListCtrl(wxWindow* parent, FontListModel* model)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_NO_HEADER),
        model(model), painter(model)
    {
        painter.onFilterChanged = [this] { if (onFilterChanged) onFilterChanged(); };
        painter.onRepaint = [this] { Refresh(); };

        InsertColumn(0, _("Font"));
        InsertColumn(1, _("Preview"));

        SetColumnWidth(0, 250);
        SetColumnWidth(1, 350);

        SetFont(painter.RowFont());
        SetItemCount(model->filtered.size());

        SetDoubleBuffered(true);
        SetToolTip(nullptr);

        Bind(wxEVT_SIZE, &FontListCtrl::OnResize, this);
        Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &FontListCtrl::OnRightClick, this);
        Bind(wxEVT_LIST_ITEM_SELECTED, &FontListCtrl::OnSelected, this);
        Bind(wxEVT_LIST_ITEM_ACTIVATED, &FontListCtrl::OnItemActivated, this);

        UpdateColumns();

        HWND hwnd = (HWND)GetHandle();

        DWORD ex = ListView_GetExtendedListViewStyle(hwnd);
        ex &= ~LVS_EX_INFOTIP;

        ListView_SetExtendedListViewStyle(hwnd, ex);

        HWND tooltip = (HWND)SendMessage(hwnd, LVM_GETTOOLTIPS, 0, 0);
        if (tooltip) {
            ShowWindow(tooltip, SW_HIDE);
        }
    }

    void RefreshModel() {
        long top = GetTopItem();

        painter.InvalidateTints();
        attr_cache_name.clear();
        SetItemCount(model->filtered.size());
        Refresh();

        SetItemState(-1, 0, wxLIST_STATE_SELECTED);

        if (!model->filtered.empty()) {
            if (top >= 0 && top < (long)model->filtered.size())
                EnsureVisible(top);
            else
                EnsureVisible(0);
        }
    }

    wxString GetSelectedFont()
    {
        long item = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

        if (item >= 0 && item < (long)model->filtered.size())
            return model->filtered[item];

        return "";
    }

    void SelectFont(wxString const& name)
    {
        ClearSelection();

        for (size_t i = 0; i < model->filtered.size(); ++i) {
            if (model->filtered[i].CmpNoCase(name) == 0) {
                SetItemState(i,
                    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);

                EnsureVisible(i);
                break;
            }
        }
    }

    void ClearSelection()
    {
        long item = -1;

        while (true) {
            item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
            if (item == -1)
                break;

            SetItemState(item, 0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        }
    }

    void UpdateColumns()
    {
        if (!GetItemCount())
            return;

        int w = GetClientSize().GetWidth();

        SetColumnWidth(0, w / 2 + 1);
        SetColumnWidth(1, w / 2 + 1);
    }

    void SetSampleSize(int size)
    {
        painter.SetSampleSize(size);
        attr_cache_name.clear();
        SetFont(painter.RowFont());
        Refresh();
    }

    void InvalidateTints()
    {
        painter.InvalidateTints();
        attr_cache_name.clear();
        Refresh();
    }

private:
    void OnSelected(wxListEvent&) { if (onSelectionChanged) onSelectionChanged(); }
    void OnItemActivated(wxListEvent&) { if (onActivated) onActivated(); }

    void OnRightClick(wxListEvent& evt)
    {
        long item = evt.GetIndex();

        if (item < 0 || item >= (long)model->filtered.size())
            return;

        painter.ShowContextMenu(this, model->filtered[item]);
    }

    void OnResize(wxSizeEvent& evt)
    {
        evt.Skip();

        CallAfter([this]
        {
            UpdateColumns();
        });
    }

    wxString OnGetItemText(long item, long column) const override
    {
        if (item < 0 || item >= (long)model->filtered.size())
            return "";

        // Column 1 is drawn below, so it deliberately has no text.
        if (column == 0)
            return painter.RowName(model->filtered[item]);

        return "";
    }

    wxListItemAttr* OnGetItemColumnAttr(long item, long column) const override
    {
        if (column != 0 || item < 0 || item >= (long)model->filtered.size())
            return nullptr;

        wxString const& face = model->filtered[item];

        auto found = attr_cache_name.find(face);
        if (found != attr_cache_name.end())
            return found->second.get();

        auto attr = std::make_unique<wxItemAttr>();
        // The row is as tall as the sample needs, but the name is not drawn at that
        // size: the slider is for the preview.
        wxFont font = painter.NameFont();

        if (FontRowPainter::IsFavorite(face)) {
            font.SetWeight(wxFONTWEIGHT_BOLD);
            attr->SetTextColour(FontRowPainter::FavoriteColour());
        }

        attr->SetFont(font);

        wxItemAttr* raw = attr.get();
        attr_cache_name.emplace(face, std::move(attr));

        return raw;
    }

    /// The preview cell is drawn *after* the list has drawn it, not instead of it.
    /// That way the cell keeps whatever background the theme gives it, including the
    /// selection - which on a modern Windows is not the plain highlight colour but a
    /// themed band, and picking a colour by hand never matches it. The sample is
    /// then composed on top through its alpha.
    ///
    /// wxWidgets uses this notification itself to apply the per-item font and
    /// colours the name column needs, so its handler runs first and this only adds
    /// flags to what it decided.
    bool MSWOnNotify(int idCtrl, WXLPARAM lParam, WXLPARAM* result) override
    {
        NMHDR* header = (NMHDR*)lParam;
        if (!header || header->code != NM_CUSTOMDRAW)
            return wxListCtrl::MSWOnNotify(idCtrl, lParam, result);

        NMLVCUSTOMDRAW* draw = (NMLVCUSTOMDRAW*)lParam;
        DWORD stage = draw->nmcd.dwDrawStage;

        if (stage == (CDDS_ITEMPOSTPAINT | CDDS_SUBITEM)) {
            if (draw->iSubItem == 1)
                DrawPreviewCell(draw);
            *result = CDRF_DODEFAULT;
            return true;
        }

        if (!wxListCtrl::MSWOnNotify(idCtrl, lParam, result))
            *result = CDRF_DODEFAULT;

        // The flags are additive, so whatever wxWidgets asked for stays asked for.
        if (stage == CDDS_PREPAINT) {
            *result |= CDRF_NOTIFYITEMDRAW;
            return true;
        }
        if (stage == CDDS_ITEMPREPAINT) {
            *result |= CDRF_NOTIFYSUBITEMDRAW;
            return true;
        }
        if (stage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
            if (draw->iSubItem == 1)
                *result |= CDRF_NOTIFYPOSTPAINT;
            return true;
        }

        return true;
    }

    void DrawPreviewCell(NMLVCUSTOMDRAW* draw)
    {
        long item = (long)draw->nmcd.dwItemSpec;
        if (item < 0 || item >= (long)model->filtered.size())
            return;

        HWND hwnd = (HWND)GetHandle();
        RECT cell{};
        if (!ListView_GetSubItemRect(hwnd, item, 1, LVIR_BOUNDS, &cell))
            return;

        bool selected = (ListView_GetItemState(hwnd, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        // The colour the list itself is using for this row's text, so the sample
        // reads the same way the name beside it does.
        wxColour ink = selected
            ? wxColour(GetRValue(draw->clrText), GetGValue(draw->clrText), GetBValue(draw->clrText))
            : GetForegroundColour();

        wxBitmap const* bitmap = painter.PreviewFor(model->filtered[item], ink, !selected);
        if (!bitmap || !bitmap->IsOk())
            return;

        int width = std::min((int)(cell.right - cell.left) - 4, bitmap->GetWidth());
        int height = std::min((int)(cell.bottom - cell.top), bitmap->GetHeight());
        if (width <= 0 || height <= 0)
            return;

        HDC hdc = (HDC)draw->nmcd.hdc;
        HDC memory = CreateCompatibleDC(hdc);
        HBITMAP previous = (HBITMAP)SelectObject(memory, (HBITMAP)bitmap->GetHBITMAP());

        // GdiAlphaBlend rather than AlphaBlend: same function, exported by gdi32,
        // so nothing extra has to be linked. wxWidgets keeps an alpha bitmap
        // premultiplied on this platform, which is what AC_SRC_ALPHA expects.
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        GdiAlphaBlend(hdc, cell.left + 2,
            cell.top + ((cell.bottom - cell.top) - height) / 2, width, height,
            memory, 0, 0, width, height, blend);

        SelectObject(memory, previous);
        DeleteDC(memory);
    }

    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override
    {
        if (message == WM_NOTIFY) {
            NMHDR* header = (NMHDR*)lParam;

            if (header && header->code == TTN_GETDISPINFO) {
                return 0;
            }
        }

        return wxListCtrl::MSWWindowProc(message, wParam, lParam);
    }
};

#else

/// Two columns of one row each: the face name, and the sample drawn in that face.
///
/// A wxVListBox because the preview is a bitmap rather than text, and this is the
/// virtual list wxWidgets lets us paint ourselves on every platform. Windows keeps
/// its native wxListCtrl above; everywhere else shares this one.
class FontListCtrl : public wxVListBox {
    FontListModel* model;
    FontRowPainter painter;

public:
    std::function<void()> onFilterChanged;
    std::function<void()> onSelectionChanged;
    std::function<void()> onActivated;

    FontListCtrl(wxWindow* parent, FontListModel* model)
        : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLB_SINGLE),
        model(model), painter(model)
    {
        painter.onFilterChanged = [this] { if (onFilterChanged) onFilterChanged(); };
        painter.onRepaint = [this] { Refresh(); };

        SetDoubleBuffered(true);

        Bind(wxEVT_RIGHT_DOWN, &FontListCtrl::OnRightDown, this);
        Bind(wxEVT_LISTBOX, &FontListCtrl::OnSelected, this);
        Bind(wxEVT_LISTBOX_DCLICK, &FontListCtrl::OnItemActivated, this);

        SetItemCount(model->filtered.size());
    }

    void RefreshModel() {
        size_t top = GetVisibleRowsBegin();

        painter.InvalidateTints();
        SetItemCount(model->filtered.size());
        SetSelection(wxNOT_FOUND);
        Refresh();

        if (!model->filtered.empty())
            ScrollToRow(top < model->filtered.size() ? top : 0);
    }

    wxString GetSelectedFont()
    {
        int item = GetSelection();

        if (item >= 0 && item < (int)model->filtered.size())
            return model->filtered[item];

        return "";
    }

    void SelectFont(wxString const& name)
    {
        ClearSelection();

        for (size_t i = 0; i < model->filtered.size(); ++i) {
            if (model->filtered[i].CmpNoCase(name) == 0) {
                SetSelection((int)i);
                ScrollToRow(i);
                break;
            }
        }
    }

    void ClearSelection()
    {
        SetSelection(wxNOT_FOUND);
    }

    /// Kept so the dialog's resize handler does not have to know which control it
    /// has; the two column widths are worked out per paint from the client width.
    void UpdateColumns()
    {
        Refresh();
    }

    void SetSampleSize(int size)
    {
        painter.SetSampleSize(size);
        SetItemCount(model->filtered.size());
        Refresh();
    }

    void InvalidateTints()
    {
        painter.InvalidateTints();
        Refresh();
    }

private:
    void OnSelected(wxCommandEvent&) { if (onSelectionChanged) onSelectionChanged(); }
    void OnItemActivated(wxCommandEvent&) { if (onActivated) onActivated(); }

    void OnRightDown(wxMouseEvent& evt)
    {
        int item = VirtualHitTest(evt.GetPosition().y);

        if (item == wxNOT_FOUND || item < 0 || item >= (int)model->filtered.size())
            return;

        SetSelection(item);
        painter.ShowContextMenu(this, model->filtered[item]);
    }

    wxCoord OnMeasureItem(size_t) const override
    {
        return painter.RowHeight();
    }

    void OnDrawItem(wxDC& dc, wxRect const& rect, size_t n) const override
    {
        if (n >= model->filtered.size())
            return;

        wxString const& face = model->filtered[n];
        bool selected = (int)n == GetSelection();

        // OnDrawBackground has already filled the row, including the highlight on a
        // selected one, so only the text colour is needed here.
        wxColour name_ink = selected
            ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
            : GetForegroundColour();

        // The same halves the native columns use, worked out per paint so a resize
        // needs nothing but a repaint.
        int half = rect.width / 2;

        // Only the row height follows the sample size; the name keeps the interface
        // size, so moving the slider moves the preview and nothing else.
        wxFont font = painter.NameFont();
        if (FontRowPainter::IsFavorite(face)) {
            font.SetWeight(wxFONTWEIGHT_BOLD);
            if (!selected) name_ink = FontRowPainter::FavoriteColour();
        }

        dc.SetFont(font);
        dc.SetTextForeground(name_ink);

        {
            // A long face name must not run into the preview half.
            wxDCClipper clip(dc, wxRect(rect.x + 2, rect.y, half - 4, rect.height));
            dc.DrawText(painter.RowName(face), rect.x + 2,
                rect.y + (rect.height - dc.GetCharHeight()) / 2);
        }

        // Composed over the row background OnDrawBackground already drew, selection
        // included, in the same text colour the name is using.
        painter.DrawPreview(dc, wxRect(rect.x + half + 2, rect.y, half - 4, rect.height),
            face, selected
                ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
                : GetForegroundColour(),
            !selected);
    }

    /// The horizontal rule wxLC_HRULES gives the native list.
    void OnDrawSeparator(wxDC& dc, wxRect& rect, size_t) const override
    {
        wxColour line = wxSystemSettings::GetColour(wxSYS_COLOUR_3DLIGHT);
        dc.SetPen(wxPen(line));
        dc.DrawLine(rect.x, rect.GetBottom(), rect.GetRight(), rect.GetBottom());
        rect.height -= 1;
    }
};

#endif

class ImportFontsDialog : public wxDialog {
public:
    ImportFontsDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _("Import from the subtitle"), wxDefaultPosition, wxSize(300, 100))
    {
        auto sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, _("Collecting fonts...")), 1, wxALIGN_CENTER | wxALL, 20);

        SetSizerAndFit(sizer);
        Centre();
    }
};

class DialogFontPicker : public wxDialog {
    AssFile* subs;
    FontCategorySidebar* sidebar;
    FontListModel model;
    wxSearchCtrl* search;
    wxButton* clear_cache_btn;
    wxSlider* sample_size_slider;
    FontListCtrl* font_list;
    wxCheckBox* bold_cb;
    wxCheckBox* italic_cb;
    wxCheckBox* underline_cb;
    wxCheckBox* strike_cb;
    wxCheckBox* show_at_cb;
    std::vector<wxCheckBox*> language_cbs;
    wxStaticText* filter_hint = nullptr;
    wxStaticText* preview;
    int preview_size;

    wxFont initial_font;
    wxFont current_font;
    NumericSliderCtrl* fs;
    NumericSliderCtrl* fscx;
    NumericSliderCtrl* fscy;
    NumericSliderCtrl* fsp;
    FontSizeObject initialSizeObject;

    wxFont lastSentFont;
    FontSizeObject lastSentSizeObject;

    wxString initialFaceName;
    wxString currentFaceName;
    wxString lastSentFaceName;

    std::function<void(wxString const&, wxFont const&, FontSizeObject const&, LineChangeFlags const&)> callback;
    bool callbackOnlyOnConfirm = false;

public:
    DialogFontPicker(
        wxWindow* parent,
        wxString const& initialFace,
        wxFont const& initial,
        FontSizeObject const& sizeObj,
        AssFile* subs,
        std::function<void(wxString const&, wxFont const&, FontSizeObject const&, LineChangeFlags const&)> cb,
        bool onlyOnConfirm = false)
        : wxDialog(parent, wxID_ANY, _("Font picker"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        initialFaceName(initialFace),
        currentFaceName(initialFace),
        lastSentFaceName(initialFace),
        initial_font(initial),
        current_font(initial),
        lastSentFont(initial),
        initialSizeObject(sizeObj),
        lastSentSizeObject(sizeObj),
        subs(subs),
        callback(cb),
        callbackOnlyOnConfirm(onlyOnConfirm)
    {
        LoadFontCache();
        LoadLists();
        LoadLanguageFilters();

        preview_size = OPT_GET("Tool/Font Picker/Preview Size")->GetInt();

        model.LoadFonts();
        model.CleanInvalidFonts();
        model.initial_face = initialFaceName;
        model.Filter("");

        BuildUI();
        BindEvents();

        font_list->SelectFont(initialFaceName);

        UpdatePreview();
        SetMinSize(wxSize(600, 350));

        CallAfter([this]{
            search->SetFocus();
        });
    }

    void ApplyFilter()
    {
        model.Filter(search->GetValue());
        font_list->RefreshModel();

        SelectBestFont();
    }

    void SelectBestFont()
    {
        wxString current = currentFaceName;
        wxString initial = initialFaceName;

        if (!current.empty()) {
            font_list->SelectFont(current);

            if (!font_list->GetSelectedFont().empty())
                return;
        }

        if (!initial.empty()) {
            font_list->SelectFont(initial);

            if (!font_list->GetSelectedFont().empty())
                return;
        }

        font_list->ClearSelection();
    }
private:
    void BuildUI()
    {
        auto* main = new wxBoxSizer(wxVERTICAL);
        auto* top = new wxBoxSizer(wxHORIZONTAL);
        auto* right = new wxBoxSizer(wxVERTICAL);

        sidebar = new FontCategorySidebar(this);
        sidebar->SetMinSize(wxSize(150, -1));

        top->Add(sidebar, 0, wxEXPAND | wxALL, 4);
        top->Add(right, 1, wxEXPAND);

        main->Add(top, 1, wxEXPAND);

        search = new wxSearchCtrl(this, wxID_ANY);
        clear_cache_btn = new wxButton(this, wxID_ANY, _("Refresh"));
        auto* preview_label = new wxStaticText(this, wxID_ANY, _("Preview size")+":");
        // The maximum has to stay in step with kPreviewMasterSize, which is what the
        // previews are rasterised at: above it the rows would be enlarging coverage.
        sample_size_slider = new wxSlider(this, wxID_ANY, OPT_GET("Tool/Font Picker/Sample Size")->GetInt(), 6, kPreviewMasterSize, wxDefaultPosition, wxSize(120,-1));

        auto* top_row = new wxBoxSizer(wxHORIZONTAL);
        top_row->Add(search, 1, wxEXPAND | wxRIGHT, 8);
        top_row->Add(clear_cache_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        top_row->Add(preview_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        top_row->Add(sample_size_slider, 0, wxALIGN_CENTER_VERTICAL);

        right->Add(top_row, 0, wxEXPAND | wxALL, 6);

        font_list = new FontListCtrl(this, &model);
        right->Add(font_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

        bold_cb = new wxCheckBox(this, wxID_ANY, _("Bold"));
        italic_cb = new wxCheckBox(this, wxID_ANY, _("Italics"));
        underline_cb = new wxCheckBox(this, wxID_ANY, _("Underline"));
        strike_cb = new wxCheckBox(this, wxID_ANY, _("Strikeout"));

        show_at_cb = new wxCheckBox(this, wxID_ANY, _("Show @ fonts"));
        show_at_cb->SetValue(show_at_fonts);

        auto* style_row = new wxBoxSizer(wxHORIZONTAL);
        style_row->Add(bold_cb, 0, wxRIGHT, 10);
        style_row->Add(italic_cb, 0, wxRIGHT, 10);
        style_row->Add(underline_cb, 0, wxRIGHT, 10);
        style_row->Add(strike_cb, 0);

        // One checkbox per configured language filter, in the configured order.
        for (size_t i = 0; i < g_language_filters.size(); ++i) {
            auto* cb = new wxCheckBox(this, wxID_ANY, g_language_filters[i].label);
            cb->SetValue(g_language_filters[i].enabled);
            cb->Bind(wxEVT_CHECKBOX, [this, i](wxCommandEvent& evt) {
                g_language_filters[i].enabled = evt.IsChecked();
                model.Filter(search->GetValue());
                font_list->RefreshModel();
            });
            language_cbs.push_back(cb);
            style_row->Add(cb, 0, wxLEFT, 10);
        }
        style_row->Add(show_at_cb, 0, wxLEFT, 15);

        right->Add(style_row, 0, wxALIGN_CENTER | wxTOP, 4);

        filter_hint = new wxStaticText(this, wxID_ANY,
            _("You can change the filter options in the Settings/Interface menu."));
        filter_hint->SetForegroundColour(
            wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        right->Add(filter_hint, 0, wxALIGN_CENTER | wxTOP, 5);
        right->AddSpacer(12);

        preview = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(OPT_GET("Tool/Font Picker/Preview")->GetString()), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        preview->SetMinSize(wxSize(-1, 130));
        preview->SetMaxSize(wxSize(-1, 130));

        main->Add(preview, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 15);

        fs   = new NumericSliderCtrl(this, "fs", initialSizeObject.fs, [&](double v) { RunCallback(); });
        fscx = new NumericSliderCtrl(this, "fscx", initialSizeObject.fscx, [&](double v) { RunCallback(); });
        fscy = new NumericSliderCtrl(this, "fscy", initialSizeObject.fscy, [&](double v) { RunCallback(); });
        fsp  = new NumericSliderCtrl(this, "fsp", initialSizeObject.fsp, [&](double v) { RunCallback(); });

        auto* slider_row = new wxBoxSizer(wxHORIZONTAL);
        slider_row->Add(fs, 0, wxRIGHT, 40);
        slider_row->Add(fscx, 0, wxRIGHT, 40);
        slider_row->Add(fscy, 0, wxRIGHT, 40);
        slider_row->Add(fsp, 0);

        main->Add(slider_row, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 5);

        auto* buttons = CreateButtonSizer(wxOK | wxCANCEL);
        main->Add(buttons, 0, wxEXPAND | wxALL, 10);

        SetSizer(main);
        UpdatePreview();

        font_list->onFilterChanged = [this]() {
            ApplyFilter();
        };

        sidebar->onCategoryChanged = [this](FontCategory cat) {
            search->SetValue("");
            ApplyFilter();

            bool is_all = (cat == FontCategory::All);
            for (auto* cb : language_cbs) cb->Show(is_all);
            filter_hint->Show(is_all && !language_cbs.empty());
            show_at_cb->Show(is_all);

            Layout();

            CallAfter([this]{
                search->SetFocus();
            });
        };

        sidebar->onImportFromSubs = [this](int idx) {
            ImportFontsDialog dlg(this);
            dlg.Show();

            wxYield();
            ImportFontsFromSubs(idx);

            dlg.Destroy();
        };

        sidebar->SelectCurrentItem();
    }

    void BindEvents()
    {
        search->Bind(wxEVT_TEXT, &DialogFontPicker::OnSearch, this);
        clear_cache_btn->Bind(wxEVT_BUTTON, &DialogFontPicker::OnClearCache, this);
        sample_size_slider->Bind(wxEVT_SLIDER, &DialogFontPicker::OnSampleSize, this);

        // Callbacks rather than Bind: the two list controls send different native
        // events, and which one is compiled in is not the dialog's business.
        font_list->onSelectionChanged = [this] { OnFontChanged(); };
        font_list->onActivated = [this] { OnFontActivated(); };

        bold_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnStyleChanged, this);
        italic_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnStyleChanged, this);
        underline_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnStyleChanged, this);
        strike_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnStyleChanged, this);
        show_at_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnToggleAtFonts, this);

        Bind(wxEVT_BUTTON, &DialogFontPicker::OnOK, this, wxID_OK);
        Bind(wxEVT_BUTTON, &DialogFontPicker::OnCancel, this, wxID_CANCEL);
        Bind(wxEVT_CLOSE_WINDOW, &DialogFontPicker::OnClose, this);
        Bind(wxEVT_CHAR_HOOK, &DialogFontPicker::OnKeyDown, this);

        preview->Bind(wxEVT_MOUSEWHEEL, &DialogFontPicker::OnPreviewWheel, this);
    }

    void OnSearch(wxCommandEvent&)
    {
        ApplyFilter();
    }

    /// Only one thing to offer here. Opening the dialog already re-enumerates the
    /// installed faces and renders whatever the caches have not seen, so a face
    /// added since the last open needs no button. What that cannot notice is a font
    /// file replaced under a name that was already known: nothing about the name
    /// changed, so every cached answer still looks current. Throwing the lot away
    /// is the only way to pick that up, and it costs the full pass again.
    void OnClearCache(wxCommandEvent&)
    {
        if (wxMessageBox(
                _("Rebuild the preview cache for every installed font?\n\n"
                  "Fonts installed since the last time are picked up on their own, so this "
                  "is only needed when a font was replaced without its name changing. It "
                  "takes a while."),
                _("Refresh"), wxYES_NO | wxICON_QUESTION, this) != wxYES)
            return;

        g_glyph_cache.clear();
        g_preview_store.Forget();
        g_font_cache_dirty = true;

        RebuildFontList();
    }

    void RebuildFontList()
    {
        model.LoadFonts();
        model.CleanInvalidFonts();
        model.Filter(search->GetValue());

        font_list->RefreshModel();
        SelectBestFont();
    }

    void OnSampleSize(wxCommandEvent&)
    {
        int size = sample_size_slider->GetValue();
        font_list->SetSampleSize(size);

        OPT_SET("Tool/Font Picker/Sample Size")->SetInt(size);
    }

    void OnFontChanged()
    {
        wxString face = font_list->GetSelectedFont();

        if (!face.empty()) {
            wxFont test(
                current_font.GetPointSize(),
                wxFONTFAMILY_DEFAULT,
                current_font.GetStyle(),
                current_font.GetWeight(),
                current_font.GetUnderlined(),
                face
            );

            test.SetStrikethrough(current_font.GetStrikethrough());

            // ignore invalid font
            if (!test.IsOk())
                return;

            current_font = test;
            currentFaceName = face;

            bold_cb->SetValue(current_font.GetWeight() == wxFONTWEIGHT_BOLD);
            italic_cb->SetValue(current_font.GetStyle() == wxFONTSTYLE_ITALIC);
            underline_cb->SetValue(current_font.GetUnderlined());
            strike_cb->SetValue(current_font.GetStrikethrough());

            UpdatePreview();
            RunCallback();
        }
    }

    void OnFontActivated()
    {
        CommitSelection();
        RunCallback(false, true);
        EndModal(wxID_OK);
    }

    void OnStyleChanged(wxCommandEvent&)
    {
        current_font.SetWeight(bold_cb->GetValue() ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        current_font.SetStyle(italic_cb->GetValue() ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
        current_font.SetUnderlined(underline_cb->GetValue());
        current_font.SetStrikethrough(strike_cb->GetValue());

        UpdatePreview();
        RunCallback();
    }

    void OnToggleAtFonts(wxCommandEvent&)
    {
        show_at_fonts = show_at_cb->GetValue();

        model.Filter(search->GetValue());
        font_list->RefreshModel();
    }

    void UpdatePreview()
    {
        wxFont preview_font(
            preview_size,
            wxFONTFAMILY_DEFAULT,
            current_font.GetStyle(),
            current_font.GetWeight(),
            current_font.GetUnderlined(),
            currentFaceName
        );

        preview_font.SetStrikethrough(current_font.GetStrikethrough());

        preview->Freeze();
        preview->SetFont(preview_font);
        preview->InvalidateBestSize();
        preview->GetParent()->Layout();
        preview->Thaw();
        preview->Refresh();
    }

    void OnPreviewWheel(wxMouseEvent& evt)
    {
        int rot = evt.GetWheelRotation();

        if (rot > 0)
            preview_size++;
        else
            preview_size--;

        preview_size = std::max(6, std::min(preview_size, 100));
        OPT_SET("Tool/Font Picker/Preview Size")->SetInt(preview_size);

        UpdatePreview();
    }

    void OnKeyDown(wxKeyEvent& evt)
    {
        int key = evt.GetKeyCode();

        if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
            CommitSelection();
            RunCallback(false, true);
            EndModal(wxID_OK);

            return;
        }

        if (key == WXK_ESCAPE) {
            if (!ConfirmCloseIfTemporary())
                return;

            RunCallback(true, true);
            EndModal(wxID_CANCEL);

            return;
        }

        if ((key == WXK_DOWN || key == WXK_TAB) && search->HasFocus()) {
            if (font_list->GetItemCount() > 0) {
                font_list->SelectFont(model.filtered[0]);
                font_list->SetFocus();
            }

            return;
        }

        evt.Skip();
    }

    bool ConfirmCloseIfTemporary()
    {
        if (g_temporary.empty())
            return true;

        return wxMessageBox(
            _("Are you sure you want to close? You have temporary saved fonts."),
            _("Confirm"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this
        ) == wxYES;
    }

    void OnCancel(wxCommandEvent&)
    {
        if (!ConfirmCloseIfTemporary())
            return;

        // Cancelling drops the chosen font, not what the dialog learned about the
        // installed ones: the install dates picked up by LoadFonts are as good
        // here as after an OK.
        SaveFontCache();
        RunCallback(true, true);
        EndModal(wxID_CANCEL);
    }

    void OnClose(wxCloseEvent& evt)
    {
        if (!ConfirmCloseIfTemporary()) {
            evt.Veto();
            return;
        }

        SaveFontCache();
        RunCallback(true, true);
        EndModal(wxID_CANCEL);
    }

    void OnOK(wxCommandEvent&)
    {
        CommitSelection();
        RunCallback(false, true);
        EndModal(wxID_OK);
    }

    void RunCallback(bool initial = false, bool last = false)
    {
        if (!callback)
            return;

        if (!last && callbackOnlyOnConfirm)
            return;

        FontSizeObject currentSizeObject;
        currentSizeObject.fs   = fs->GetValue();
        currentSizeObject.fscx = fscx->GetValue();
        currentSizeObject.fscy = fscy->GetValue();
        currentSizeObject.fsp  = fsp->GetValue();

        LineChangeFlags flags;
        wxFont font = initial ? initial_font : current_font;
        wxString face = initial ? initialFaceName : currentFaceName;
        FontSizeObject sizeObject = initial ? initialSizeObject : currentSizeObject;

        flags.fn = face != lastSentFaceName;
        flags.b  = font.GetWeight() != lastSentFont.GetWeight();
        flags.i  = font.GetStyle() != lastSentFont.GetStyle();
        flags.u  = font.GetUnderlined() != lastSentFont.GetUnderlined();
        flags.s  = font.GetStrikethrough() != lastSentFont.GetStrikethrough();

        flags.fs   = sizeObject.fs   != lastSentSizeObject.fs;
        flags.fscx = sizeObject.fscx != lastSentSizeObject.fscx;
        flags.fscy = sizeObject.fscy != lastSentSizeObject.fscy;
        flags.fsp  = sizeObject.fsp  != lastSentSizeObject.fsp;

        if (!flags.Any())
            return;

        lastSentFont = font;
        lastSentSizeObject = sizeObject;
        lastSentFaceName = face;

        callback(face, font, sizeObject, flags);
    }

    void CommitSelection()
    {
        wxString font = currentFaceName;
        if (font.empty())
            return;

        std::string key = font.ToStdString();

        g_usage_cache[key]++;
        g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), font), g_recent.end());
        g_recent.insert(g_recent.begin(), font);
        g_font_cache_dirty = true;

        if (g_recent.size() > 20)
            g_recent.pop_back();

        SaveLists();
        SaveFontCache();
    }

    void ImportFontsFromSubs(int idx)
    {
        if (idx < 0 || idx >= (int)g_custom_lists.size())
            return;

        auto& list = g_custom_lists[idx];
        auto fonts = CollectFontsFromSubs(subs);

        for (auto& f : fonts) {
            if (!model.FontExists(f))
                continue;

            if (std::find(list.fonts.begin(), list.fonts.end(), f) == list.fonts.end())
                list.fonts.push_back(f);
        }

        SaveLists();

        if (sidebar->onCategoryChanged)
            sidebar->onCategoryChanged(FontCategory::Custom);
    }
};

bool GetFontFromUser(wxWindow* parent, wxString initialFace, wxFont initial, FontSizeObject sizeObj, AssFile* subs, std::function<void(wxString, wxFont, FontSizeObject, LineChangeFlags)> callback, bool onlyOnConfirm)
{
    g_temporary.clear();

    DialogFontPicker dlg(parent, initialFace, initial, sizeObj, subs, callback, onlyOnConfirm);
    wxPoint last_position = wxPoint(OPT_GET("Tool/Font Picker/Dialog Pos X")->GetInt(), OPT_GET("Tool/Font Picker/Dialog Pos Y")->GetInt());
    wxSize last_size = wxSize(OPT_GET("Tool/Font Picker/Dialog Width")->GetInt(), OPT_GET("Tool/Font Picker/Dialog Height")->GetInt());

    dlg.SetSize(last_size);
    if (last_position.x == -1 && last_position.y == -1) {
        wxPoint mouse = wxGetMousePosition();
        wxRect screen = wxDisplay(wxDisplay::GetFromPoint(mouse)).GetClientArea();
        wxSize size = dlg.GetSize();

        int x = mouse.x - size.GetWidth() / 2;
        int y = mouse.y - size.GetHeight() / 2;

        x = std::max(screen.GetLeft(), std::min(x, screen.GetRight() - size.GetWidth()));
        y = std::max(screen.GetTop(), std::min(y, screen.GetBottom() - size.GetHeight()));

        dlg.Move(x, y);
    } else {
        dlg.Move(last_position);
    }

    bool ok = dlg.ShowModal() == wxID_OK;

    last_position = dlg.GetPosition();
    OPT_SET("Tool/Font Picker/Dialog Pos X")->SetInt(last_position.x);
    OPT_SET("Tool/Font Picker/Dialog Pos Y")->SetInt(last_position.y);

    last_size = dlg.GetSize();
    OPT_SET("Tool/Font Picker/Dialog Width")->SetInt(last_size.GetWidth());
    OPT_SET("Tool/Font Picker/Dialog Height")->SetInt(last_size.GetHeight());

    return ok;
}
