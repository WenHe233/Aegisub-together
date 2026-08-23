#pragma once

#include "ass_dialogue.h"
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

class AssDialogue;
class AssFile;

/// Whether this line is the rectangle an image mask is drawn on.
///
/// Such a line is a placeholder for a picture rather than something with a shape of its
/// own, so anything that reshapes drawings has to leave it alone.
bool IsImageMaskLine(const AssDialogue* d);

class SubtitleLineCombiner {
public:
    bool IsVisible(const AssDialogue* d) const;
    bool IsGroupStart(const AssDialogue* d) const;
    bool IsInGroup(const AssDialogue* d) const;
    bool IsCollapsed(const AssDialogue* d) const;
	bool IsGradientGroup(const AssDialogue* d) const;
	bool IsGlitchGroup(const AssDialogue* d) const;
	bool IsTextBoxGroup(const AssDialogue* d) const;
	std::string const& GetGroupLabel(const AssDialogue* d) const;
	std::string const& GetGradientDescription(const AssDialogue* d) const;
	std::string const& GetGlitchDescription(const AssDialogue* d) const;

    int GetGroupSize(const AssDialogue* d) const;
    AssDialogue* GetLastInGroup(const AssDialogue* d) const;
    const std::vector<AssDialogue*>& GetGroupLines(const AssDialogue* d) const;
	/// Add every row belonging to a selected gradient or textbox object.
	void ExpandTypesettingSelection(std::set<AssDialogue*>& selection) const;

	void Rebuild(const std::vector<AssDialogue*>& lines, AssFile const& file);
    void Toggle(AssDialogue* d);

private:
    struct ImageMaskGroup {
        bool collapsed = true;
		bool gradient = false;
		bool glitch = false;
		bool textbox = false;
		AssDialogue* start = nullptr;
		std::string label;
		std::string gradient_description;
		std::string glitch_description;
        std::vector<AssDialogue*> lines;
    };

    std::vector<ImageMaskGroup> groups;
    std::unordered_map<AssDialogue*, int> lookup;
};
