#include "subtitle_line_combiner.h"

#include "ass_file.h"
#include "typesetting_gradient.h"
#include "typesetting_textbox.h"

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

void SubtitleLineCombiner::Rebuild(const std::vector<AssDialogue*>& lines, AssFile const& file) {
    struct OpenGroup {
        bool gradient = false;
		bool textbox = false;
        std::vector<AssDialogue*> lines;
		int start_row = -1;
    };

    std::vector<OpenGroup> open_groups;

    for (auto& g : groups) {
        if (!g.collapsed && !g.lines.empty())
			open_groups.push_back({ g.gradient, g.textbox, g.lines,
				g.start ? g.start->Row : -1 });
    }

    groups.clear();
    lookup.clear();

    const int MIN_SEQUENCE = 10;
    int i = 0;
    int n = (int)lines.size();

    while (i < n) {
        bool gradient = IsGradientStart(file, lines[i]);
		bool textbox = typesetting::textbox::IsSource(file, lines[i]);
        if (!gradient && !textbox && !IsImageMaskLine(lines[i])) {
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
        else if (textbox) {
			++j;
			while (j < n && HasSameTiming(base, lines[j]) &&
				typesetting::textbox::IsEffect(lines[j]) &&
				!typesetting::textbox::IsSource(file, lines[j]))
				++j;
		}
        else {
            while (j < n && HasSameTiming(base, lines[j]) && IsImageMaskLine(lines[j]))
                j++;
        }

        int count = j - start;
        if (count >= (gradient || textbox ? 1 : MIN_SEQUENCE)) {
            groups.emplace_back();
            int idx = (int)groups.size() - 1;

            auto& g = groups.back();
            g.start = lines[start];
            g.collapsed = true;
            g.gradient = gradient;
			g.textbox = textbox;
			if (gradient) {
				g.label = GradientLabel(file, base);
				g.gradient_description = typesetting::gradient::GroupDescription(file, *base);
			}
			else if (textbox)
				g.label = typesetting::textbox::Label(file, *base);

            for (int k = start; k < j; ++k) {
                g.lines.push_back(lines[k]);
                lookup[lines[k]] = idx;
            }

            i = j;
        }
        else {
            i = j;
        }
    }

	// Preserve expanded state one-to-one. First use surviving row pointers, which
	// keeps identical adjacent gradients distinct across ordinary metadata edits.
	// If a tool regenerated all rows, fall back to the content match and nearest
	// position, still consuming each new group at most once.
	std::vector<bool> restored(groups.size(), false);
	std::vector<bool> matched_open(open_groups.size(), false);
	for (size_t old_index = 0; old_index < open_groups.size(); ++old_index) {
		auto const& old = open_groups[old_index];
		std::unordered_set<AssDialogue*> old_lines(old.lines.begin(), old.lines.end());
		size_t best = groups.size();
		size_t best_shared = 0;
		for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
			auto const& group = groups[group_index];
			if (restored[group_index] || old.gradient != group.gradient ||
				old.textbox != group.textbox) continue;
			size_t shared = 0;
			for (auto line : group.lines)
				if (old_lines.count(line)) ++shared;
			if (shared > best_shared) {
				best = group_index;
				best_shared = shared;
			}
		}
		if (best != groups.size()) {
			groups[best].collapsed = false;
			restored[best] = true;
			matched_open[old_index] = true;
		}
	}
	for (size_t old_index = 0; old_index < open_groups.size(); ++old_index) {
		if (matched_open[old_index]) continue;
		auto const& old = open_groups[old_index];
		size_t best = groups.size();
		int best_distance = 0;
		for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
			auto const& group = groups[group_index];
			if (restored[group_index] || old.gradient != group.gradient ||
				old.textbox != group.textbox || !IsSameGroup(old.lines, group.lines)) continue;
			int row = group.start ? group.start->Row : -1;
			int distance = old.start_row > row ? old.start_row - row : row - old.start_row;
			if (best == groups.size() || distance < best_distance) {
				best = group_index;
				best_distance = distance;
			}
		}
		if (best != groups.size()) {
			groups[best].collapsed = false;
			restored[best] = true;
		}
	}
}

bool SubtitleLineCombiner::IsVisible(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end())
        return true;

    const auto& g = groups[it->second];

    if (!g.collapsed)
        return true;

    return d == g.start;
}

bool SubtitleLineCombiner::IsGroupStart(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    return it != lookup.end() && groups[it->second].start == d;
}

bool SubtitleLineCombiner::IsInGroup(const AssDialogue* d) const {
    return lookup.find(const_cast<AssDialogue*>(d)) != lookup.end();
}

bool SubtitleLineCombiner::IsCollapsed(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end()) return false;
    return groups[it->second].collapsed;
}

int SubtitleLineCombiner::GetGroupSize(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end()) return 1;
    return (int)groups[it->second].lines.size();
}

AssDialogue* SubtitleLineCombiner::GetLastInGroup(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end())
        return nullptr;

    const auto& lines = groups[it->second].lines;
    if (lines.empty())
        return nullptr;

    return lines.back();
}

const std::vector<AssDialogue*>& SubtitleLineCombiner::GetGroupLines(const AssDialogue* d) const {
    static std::vector<AssDialogue*> empty;

    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end())
        return empty;

    return groups[it->second].lines;
}

void SubtitleLineCombiner::ExpandTypesettingSelection(std::set<AssDialogue*>& selection) const {
	std::vector<AssDialogue*> selected(selection.begin(), selection.end());
	for (auto line : selected) {
		auto it = lookup.find(line);
		if (it == lookup.end()) continue;
		auto const& group = groups[it->second];
		if (!group.gradient && !group.textbox) continue;
		selection.insert(group.lines.begin(), group.lines.end());
	}
}

void SubtitleLineCombiner::Toggle(AssDialogue* d) {
    auto it = lookup.find(d);
    if (it == lookup.end()) return;

    auto& g = groups[it->second];
    g.collapsed = !g.collapsed;
}

bool SubtitleLineCombiner::IsGradientGroup(const AssDialogue* d) const {
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    return it != lookup.end() && groups[it->second].gradient;
}

bool SubtitleLineCombiner::IsTextBoxGroup(const AssDialogue* d) const {
	auto it = lookup.find(const_cast<AssDialogue*>(d));
	return it != lookup.end() && groups[it->second].textbox;
}

std::string const& SubtitleLineCombiner::GetGroupLabel(const AssDialogue* d) const {
    static std::string empty;
    auto it = lookup.find(const_cast<AssDialogue*>(d));
    if (it == lookup.end()) return empty;
	return groups[it->second].label;
}

std::string const& SubtitleLineCombiner::GetGradientDescription(const AssDialogue* d) const {
	static std::string empty;
	auto it = lookup.find(const_cast<AssDialogue*>(d));
	if (it == lookup.end()) return empty;
	return groups[it->second].gradient_description;
}
