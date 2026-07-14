package collab

import (
	"fmt"
	"testing"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
)

func TestTenThousandInsertionsInSameGapRemainOrdered(t *testing.T) {
	lines := []protocol.Line{{LineID: "left", PosKey: "left"}, {LineID: "right", PosKey: "right"}}
	canonicalizePositions(lines)
	reindexCount := 0
	for index := 0; index < 10000; index++ {
		// All generated lines stay immediately before the right anchor. Avoid
		// rescanning IDs here so this test measures rank allocation/reindexing.
		insertAt := len(lines) - 1
		position, reindexed := positionForInsert(lines, insertAt)
		if reindexed {
			reindexCount++
		}
		lines = insertLine(lines, insertAt, protocol.Line{LineID: fmt.Sprintf("line-%05d", index), PosKey: position})
	}
	if reindexCount == 0 {
		t.Fatal("dense insertion test never exercised reindexing")
	}
	for index := 1; index < len(lines); index++ {
		if lines[index-1].PosKey >= lines[index].PosKey {
			t.Fatalf("positions are not strictly ordered at %d", index)
		}
		if len(lines[index].PosKey) > 64 {
			t.Fatalf("position exceeded protocol limit: %d", len(lines[index].PosKey))
		}
	}
}

func TestInsertionAnchorFallbacksAreDeterministic(t *testing.T) {
	lines := []protocol.Line{{LineID: "a"}, {LineID: "b"}, {LineID: "c"}}
	a, b, missing := "a", "b", "missing"
	if index := insertionIndex(lines, &a, &b); index != 1 {
		t.Fatalf("valid pair inserted at %d", index)
	}
	if index := insertionIndex(lines, &b, &missing); index != 2 {
		t.Fatalf("left-only fallback inserted at %d", index)
	}
	if index := insertionIndex(lines, &missing, &b); index != 1 {
		t.Fatalf("right-only fallback inserted at %d", index)
	}
	if index := insertionIndex(lines, &missing, nil); index != len(lines) {
		t.Fatalf("missing anchors inserted at %d", index)
	}
}

func TestRankRoundTrip(t *testing.T) {
	for _, key := range []string{"0000000000000000", "Uzzzzzzzzzzzzzzz", "zzzzzzzzzzzzzzzz"} {
		value, ok := decodeRank(key)
		if !ok || encodeRank(value) != key {
			t.Fatalf("rank did not round-trip: %q", key)
		}
	}
}
