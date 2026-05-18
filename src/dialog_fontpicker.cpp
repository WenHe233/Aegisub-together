#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <ctime>
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
#include <wx/srchctrl.h>
#include <wx/stdpaths.h>
#include <wx/tokenzr.h>
#include <wx/treectrl.h>
#include <wx/wx.h>
#include <windows.h>

#include "ass_file.h"
#include "ass_dialogue.h"
#include "ass_style.h"
#include "compat.h"
#include "font_size_object.h"
#include "line_change_flags.h"
#include "options.h"
#include "ui_numeric_slider.cpp"

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
static bool filter_hungarian = true;
static bool filter_kanji = false;

static std::unordered_map<std::string, bool> g_kanji_cache;
static std::unordered_map<std::string, bool> g_hungarian_cache;
static std::unordered_map<std::string, json::Integer> g_usage_cache;
static std::unordered_map<std::string, json::Integer> g_installed_cache;
static std::vector<FontListData> g_custom_lists;
static std::unordered_set<wxString> g_temporary;
static std::unordered_set<wxString> g_favorites;
static std::vector<wxString> g_recent;

static bool g_font_cache_loaded = false;
static bool g_font_cache_dirty = false;

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

        FILE *log = fopen("app.log", "a");
        fprintf(log, "File path: %s \n\n", path.string().c_str());

        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const std::string& name = it->first;

            g_installed_cache[name] = NowTimestamp() - 7 * 24 * 60 * 60;

            try {
                json::Object& entry = it->second;

                if (entry.count("kanji")) {
                    try {
                        g_kanji_cache[name] = entry["kanji"];
                    }
                    catch (...) {}
                }

                if (entry.count("custom")) {
                    try {
                        json::Object& custom = entry["custom"];

                        if (custom.count("hungarian")) {
                            g_hungarian_cache[name] = custom["hungarian"];
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

            fprintf(log, "Font cache: %s installed_at = %lld usage = %lld \n", name.c_str(), (long long)g_installed_cache[name], (long long)g_usage_cache[name]);
        }

        fclose(log);
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

    for (auto& [font, k] : g_kanji_cache) {
        json::Object entry;

        json::Object custom;
        custom["hungarian"] = g_hungarian_cache[font];

        entry["kanji"] = k;
        entry["custom"] = std::move(custom);
        entry["usage"] = g_usage_cache[font];
        entry["installed_at"] = g_installed_cache[font];

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

        for (auto const& f : fonts) {
            FontHasKanji(f);
            FontHasHungarian(f);
        }

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

            if (filter_hungarian && !FontHasHungarian(f))
                continue;

            if (filter_kanji && !FontHasKanji(f))
                continue;

            if (t.empty() || lower_cache[f].Contains(t))
                filtered.push_back(f);
        }
    }

    bool FontExists(const wxString& name) const
    {
        return font_set.count(name) > 0;
    }

    bool FontHasKanji(wxString const& face)
    {
        auto it = g_kanji_cache.find(face.ToStdString());
        if (it != g_kanji_cache.end())
            return it->second;

        HDC hdc = GetDC(NULL);

        LOGFONTW lf{};
        wcsncpy_s(lf.lfFaceName, face.wc_str(), LF_FACESIZE - 1);
        lf.lfFaceName[LF_FACESIZE - 1] = L'\0';

        HFONT font = CreateFontIndirectW(&lf);
        HFONT old = (HFONT)SelectObject(hdc, font);

        wchar_t ch = L'漢';
        WORD glyph;

        GetGlyphIndicesW(hdc, &ch, 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS);

        bool ok = glyph != 0xFFFF;

        SelectObject(hdc, old);
        DeleteObject(font);
        ReleaseDC(NULL, hdc);

        g_kanji_cache[face.ToStdString()] = ok;
        g_font_cache_dirty = true;

        return ok;
    }

    bool FontHasHungarian(wxString const& face)
    {
        auto it = g_hungarian_cache.find(face.ToStdString());
        if (it != g_hungarian_cache.end())
            return it->second;

        HDC hdc = GetDC(NULL);

        LOGFONTW lf{};
        wcsncpy_s(lf.lfFaceName, face.wc_str(), LF_FACESIZE - 1);
        lf.lfFaceName[LF_FACESIZE - 1] = L'\0';

        HFONT font = CreateFontIndirectW(&lf);
        HFONT old = (HFONT)SelectObject(hdc, font);

        auto has_char = [&](wchar_t ch)
        {
            WORD glyph;
            GetGlyphIndicesW(hdc, &ch, 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS);
            return glyph != 0xFFFF;
        };

        bool lower_ok = has_char(L'ő') && has_char(L'ű');
        bool upper_ok = has_char(L'Ő') && has_char(L'Ű');
        bool ok = lower_ok || upper_ok;

        SelectObject(hdc, old);
        DeleteObject(font);
        ReleaseDC(NULL, hdc);

        g_hungarian_cache[face.ToStdString()] = ok;
        g_font_cache_dirty = true;

        return ok;
    }
};

class FontListCtrl : public wxListCtrl {
    FontListModel* model;
    wxString sampleText;
    int sampleSize;
    wxFont baseFont;
    mutable std::unordered_map<wxString, std::unique_ptr<wxItemAttr>> attr_cache_name;
    mutable std::unordered_map<wxString, std::unique_ptr<wxItemAttr>> attr_cache_preview;
    mutable std::unordered_map<wxString, wxFont> preview_cache;

public:
    std::function<void()> onFilterChanged;

    FontListCtrl(wxWindow* parent, FontListModel* model)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxLC_HRULES | wxLC_NO_HEADER),
        model(model)
    {
        baseFont = GetFont();
        sampleSize = OPT_GET("Tool/Font Picker/Sample Size")->GetInt();
        sampleText = wxString::FromUTF8(OPT_GET("Tool/Font Picker/Sample")->GetString());

        InsertColumn(0, _("Font"));
        InsertColumn(1, _("Preview"));

        SetColumnWidth(0, 250);
        SetColumnWidth(1, 350);

        SetItemCount(model->filtered.size());

        SetDoubleBuffered(true);
        SetToolTip(nullptr);

        Bind(wxEVT_SIZE, &FontListCtrl::OnResize, this);
        Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &FontListCtrl::OnRightClick, this);

        UpdateColumns();
        SetSampleSize(sampleSize);

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
        sampleSize = size;

        wxFont f = baseFont;
        f.SetPointSize(size);
        SetFont(f);

        attr_cache_name.clear();
        attr_cache_preview.clear();
        preview_cache.clear();
        Refresh();
    }

    void OnRightClick(wxListEvent& evt)
    {
        long item = evt.GetIndex();

        if (item < 0 || item >= (long)model->filtered.size())
            return;

        wxString font = model->filtered[item];
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

        int res = GetPopupMenuSelectionFromUser(menu);
        if (res == 1) {
            ToggleTemporary(font);
        } else if (res == 2) {
            CopyToClipboard(font);
        } else if (res == 3) {
            ToggleFavorite(font);
        } else if (res == 4) {
            RemoveFromCurrentList(font);
        } else if (res >= 1000) {
            int idx = res - 1000;
            AssignToList(font, idx);
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

        attr_cache_name.clear();

        Refresh();

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
    wxString OnGetItemText(long item, long column) const override
    {
        if (item < 0 || item >= (long)model->filtered.size())
            return "";

        if (column == 0) {
            const wxString& name = model->filtered[item];

            if (g_favorites.count(name))
                return name + wxString::FromUTF8(" ★");

            return name;
        }

        if (column == 1)
            return sampleText;

        return "";
    }

    wxListItemAttr* OnGetItemColumnAttr(long item, long column) const override
    {
        if (item < 0 || item >= (long)model->filtered.size())
            return nullptr;

        wxFont font = baseFont;
        const wxString& fontName = model->filtered[item];

        auto& cache = (column == 0) ? attr_cache_name : attr_cache_preview;

        auto it = cache.find(fontName);
        if (it != cache.end())
            return it->second.get();

        if (column == 1) {
            auto itf = preview_cache.find(fontName);

            if (itf != preview_cache.end()) {
                font = itf->second;
            } else {
                wxFont f = baseFont;
                f.SetPointSize(sampleSize);
                f.SetFaceName(fontName);

                if (f.IsOk()) {
                    preview_cache[fontName] = f;
                    font = f;
                }
            }
        }

        auto attr = std::make_unique<wxItemAttr>();

        if (g_favorites.count(fontName) > 0 && column == 0) {
            font.SetWeight(wxFONTWEIGHT_BOLD);
            attr->SetTextColour(wxColour(200, 170, 0));
        }

        attr->SetFont(font);

        wxItemAttr* raw = attr.get();
        cache.emplace(fontName, std::move(attr));

        return raw;
    }

    void OnResize(wxSizeEvent& evt)
    {
        evt.Skip();

        CallAfter([this]
        {
            UpdateColumns();
        });
    }

    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override
    {
        if (message == WM_NOTIFY) {
            NMHDR* hdr = (NMHDR*)lParam;

            if (hdr && hdr->code == TTN_GETDISPINFO) {
                return 0;
            }
        }

        return wxListCtrl::MSWWindowProc(message, wParam, lParam);
    }
};

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
    wxCheckBox* hungarian_cb;
    wxCheckBox* kanji_cb;
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
        sample_size_slider = new wxSlider(this, wxID_ANY, OPT_GET("Tool/Font Picker/Sample Size")->GetInt(), 6, 30, wxDefaultPosition, wxSize(120,-1));

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

        hungarian_cb = new wxCheckBox(this, wxID_ANY, "Magyar");
        hungarian_cb->SetValue(filter_hungarian);

        kanji_cb = new wxCheckBox(this, wxID_ANY, _("Kanji"));
        kanji_cb->SetValue(filter_kanji);

        auto* style_row = new wxBoxSizer(wxHORIZONTAL);
        style_row->Add(bold_cb, 0, wxRIGHT, 10);
        style_row->Add(italic_cb, 0, wxRIGHT, 10);
        style_row->Add(underline_cb, 0, wxRIGHT, 10);
        style_row->Add(strike_cb, 0);

        style_row->Add(hungarian_cb, 0, wxLEFT, 10);
        style_row->Add(kanji_cb, 0, wxLEFT, 10);
        style_row->Add(show_at_cb, 0, wxLEFT, 15);

        right->Add(style_row, 0, wxALIGN_CENTER | wxBOTTOM, 15);

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
            hungarian_cb->Show(is_all);
            kanji_cb->Show(is_all);
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

        font_list->Bind(wxEVT_LIST_ITEM_SELECTED, &DialogFontPicker::OnFontChanged, this);
        font_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &DialogFontPicker::OnFontActivated, this);

        bold_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnStyleChanged, this);
        italic_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnStyleChanged, this);
        underline_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnStyleChanged, this);
        strike_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnStyleChanged, this);
        show_at_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnToggleAtFonts, this);
        hungarian_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnHungarianFilter, this);
        kanji_cb->Bind(wxEVT_CHECKBOX, &DialogFontPicker::OnKanjiFilter, this);

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

    void OnClearCache(wxCommandEvent&)
    {
        g_kanji_cache.clear();
        g_hungarian_cache.clear();
        g_font_cache_dirty = true;

        model.LoadFonts();
        model.Filter(search->GetValue());

        font_list->RefreshModel();
    }

    void OnSampleSize(wxCommandEvent&)
    {
        int size = sample_size_slider->GetValue();
        font_list->SetSampleSize(size);

        OPT_SET("Tool/Font Picker/Sample Size")->SetInt(size);
    }

    void OnFontChanged(wxListEvent&)
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

    void OnFontActivated(wxListEvent&)
    {
        CommitSelection();
        RunCallback();
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

    void OnHungarianFilter(wxCommandEvent&)
    {
        filter_hungarian = hungarian_cb->GetValue();

        model.Filter(search->GetValue());
        font_list->RefreshModel();
    }

    void OnKanjiFilter(wxCommandEvent&)
    {
        filter_kanji = kanji_cb->GetValue();

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
                font_list->SetItemState(0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
                font_list->EnsureVisible(0);
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

        RunCallback(true, true);
        EndModal(wxID_CANCEL);
    }

    void OnClose(wxCloseEvent& evt)
    {
        if (!ConfirmCloseIfTemporary()) {
            evt.Veto();
            return;
        }

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