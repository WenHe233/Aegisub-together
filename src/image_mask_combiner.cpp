#include "image_mask_combiner.h"

#include "ass_file.h"
#include "typesetting_gradient.h"

#include <regex>
#include <unordered_set>

static std::string StripOverrides(const std::string& s) {
    std::string out;
    out.reserve(s.size());

    bool in_brace = false;

    for (char c : s) {
        if (c == '{') in_brace = true;
        else if (c == '}') in_brace = false;
        else if (!in_brace) out += c;
    }

    return out;
}

static std::string MakeGroupKey(const AssDialogue* l) {
    return StripOverrides(l->Text.get()) + "|" +
           std::to_string(int(l->Start)) + "|" +
           std::to_string(int(l->End));
}

static bool HasSameTiming(const AssDialogue* a, const AssDialogue* b) {
    return a->Start == b->Start && a->End == b->End;
}

static bool IsGradientLine(const AssDialogue* d) {
    return d && d->Effect.get() == "gradient-fx";
}

static std::string GradientExtra(AssFile const& file, AssDialogue const* d,
                                char const* key) {
    if (!d) return {};
    for (auto const& extra : file.GetExtradata(d->ExtradataIds))
        if (extra.key == key) return extra.value;
    return {};
}

static bool IsGradientStart(AssFile const& file, const AssDialogue* d) {
    if (!IsGradientLine(d) ||
        GradientExtra(file, d, "aegisub/gradient-fx").empty())
        return false;
    return d->Comment ||
        !GradientExtra(file, d, "aegisub/gradient-fx-source").empty();
}

static std::string GradientLabel(AssFile const& file, AssDialogue const* d) {
    auto source = GradientExtra(file, d, "aegisub/gradient-fx-source");
    if (!source.empty()) {
        try {
            return AssDialogue(source).GetStrippedText();
        }
        catch (...) { }
    }
    return d ? d->GetStrippedText() : std::string();
}

bool IsImageMaskLine(const AssDialogue* d) {
    const std::string& raw = d->Text.get();

    if (raw.find("\\p1") == std::string::npos)
        return false;

    const std::string text = StripOverrides(raw);

    if (text.find("m 0 0 l 0 ") == std::string::npos)
        return false;

    static const std::regex pattern(R"(m 0 0 l 0 \d+(?:\.\d+)? \d+(?:\.\d+)? \d+(?:\.\d+)? \d+(?:\.\d+)? 0)");

    return std::regex_search(text, pattern);
}

static bool IsSameGroup(const std::vector<AssDialogue*>& old_lines, const std::vector<AssDialogue*>& new_lines) {
    if (old_lines.empty() || new_lines.empty())
        return false;

    if (!HasSameTiming(old_lines.front(), new_lines.front()))
        return false;

    if (!HasSameTiming(old_lines.back(), new_lines.back()))
        return false;

    std::unordered_set<std::string> old_set;

    for (auto* l : old_lines)
        old_set.insert(MakeGroupKey(l));

    int matches = 0;

    for (auto* l : new_lines)
        if (old_set.count(MakeGroupKey(l)))
            matches++;

    int min_size = std::min((int)old_lines.size(), (int)new_lines.size());

    return matches >= min_size * 0.7;
}

void ImageMaskCombiner::Rebuild(const std::vector<AssDialogue*>& lines, AssFile const& file) {
    struct OpenGroup {
        bool gradient = false;
        std::vector<AssDialogue*> lines;
    };

    std::vector<OpenGroup> open_groups;

    for (auto& g : groups) {
        if (!g.collapsed && !g.lines.empty())
            open_groups.push_back({ g.gradient, g.lines });
    }

    groups.clear();
    lookup.clear();

    const int MIN_SEQUENCE = 10;
    int i = 0;
    int n = (int)lines.size();

    while (i < n) {
        bool gradient = IsGradientStart(file, lines[i]);
        if (!gradient && !IsImageMaskLine(lines[i])) {
            i++;
            continue;
        }

        int start = i;
        int j = i;

        AssDialogue* base = lines[start];

        if (gradient) {
            ++j;
            while (j < n && HasSameTiming(base, lines[j]) && IsGradientLine(lines[j]) &&
                   !IsGradientStart(file, lines[j]))
                ++j;
        }
        else {
            while (j < n && HasSameTiming(base, lines[j]) && IsImageMaskLine(lines[j]))
                j++;
        }

        int count = j - start;
        if (count >= (gradient ? 1 : MIN_SEQUENCE)) {
            groups.emplace_back();
            int idx = (int)groups.size() - 1;

            auto& g = groups.back();
            g.start = lines[start];
            g.collapsed = true;
            g.gradient = gradient;
			if (gradient) {
				g.label = GradientLabel(file, base);
				g.gradient_description = typesetting::gradient::GroupDescription(file, *base);
			}

            for (int k = start; k < j; ++k) {
                g.lines.push_back(lines[k]);
                lookup[lines[k]] = idx;
            }

            for (auto& old : open_groups) {
                if (old.gradient == g.gradient && IsSameGroup(old.lines, g.lines)) {
                    g.collapsed = false;
                    break;
                }
            }

            i = j;
        }
        else {
            i = j;
        }
    }
}

bool ImageMaskCombiner::IsVisible(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end())
        return true;

    const auto& g = groups[it->second];

    if (!g.collapsed)
        return true;

    return d == g.start;
}

bool ImageMaskCombiner::IsGroupStart(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    return it != lookup.end() && groups[it->second].start == d;
}

bool ImageMaskCombiner::IsInGroup(const AssDialogue* d) const {
    return lookup.find(const_cast<AssDialogue*>(d)) != lookup.end();
}

bool ImageMaskCombiner::IsCollapsed(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end()) return false;
    return groups[it->second].collapsed;
}

int ImageMaskCombiner::GetGroupSize(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end()) return 1;
    return (int)groups[it->second].lines.size();
}

AssDialogue* ImageMaskCombiner::GetLastInGroup(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end())
        return nullptr;

    const auto& lines = groups[it->second].lines;
    if (lines.empty())
        return nullptr;

    return lines.back();
}

const std::vector<AssDialogue*>& ImageMaskCombiner::GetGroupLines(const AssDialogue* d) const {
    static std::vector<AssDialogue*> empty;

    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end())
        return empty;

    return groups[it->second].lines;
}

void ImageMaskCombiner::Toggle(AssDialogue* d) {
    auto it = lookup.find(d);
    if (it == lookup.end()) return;

    auto& g = groups[it->second];
    g.collapsed = !g.collapsed;
}

bool ImageMaskCombiner::IsGradientGroup(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    return it != lookup.end() && groups[it->second].gradient;
}

std::string const& ImageMaskCombiner::GetGroupLabel(const AssDialogue* d) const {
    static std::string empty;
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end()) return empty;
	return groups[it->second].label;
}

std::string const& ImageMaskCombiner::GetGradientDescription(const AssDialogue* d) const {
	static std::string empty;
	auto it = lookup.find(const_cast<AssDialogue*>(d));
	if (it == lookup.end()) return empty;
	return groups[it->second].gradient_description;
}
