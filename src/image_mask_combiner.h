#pragma once

#include "ass_dialogue.h"
#include <vector>
#include <unordered_map>

class AssDialogue;

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