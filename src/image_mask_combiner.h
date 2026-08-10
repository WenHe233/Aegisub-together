#pragma once

#include "ass_dialogue.h"
#include <vector>
#include <unordered_map>

class AssDialogue;

/// Whether this line is the rectangle an image mask is drawn on.
///
/// Such a line is a placeholder for a picture rather than something with a shape of its
/// own, so anything that reshapes drawings has to leave it alone.
bool IsImageMaskLine(const AssDialogue* d);

class ImageMaskCombiner {
public:
    bool IsVisible(const AssDialogue* d) const;
    bool IsGroupStart(const AssDialogue* d) const;
    bool IsInGroup(const AssDialogue* d) const;
    bool IsCollapsed(const AssDialogue* d) const;

    int GetGroupSize(const AssDialogue* d) const;
    AssDialogue* GetLastInGroup(const AssDialogue* d) const;
    const std::vector<AssDialogue*>& GetGroupLines(const AssDialogue* d) const;

    void Rebuild(const std::vector<AssDialogue*>& lines);
    void Toggle(AssDialogue* d);

private:
    struct ImageMaskGroup {
        bool collapsed = true;
        AssDialogue* start = nullptr;
        std::vector<AssDialogue*> lines;
    };

    std::vector<ImageMaskGroup> groups;
    std::unordered_map<AssDialogue*, int> lookup;
};