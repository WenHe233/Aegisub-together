package collab

import (
	"testing"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
)

func TestValidLineID(t *testing.T) {
	tests := []struct {
		value string
		valid bool
	}{
		{"0000000000-1", true},
		{"9K3MT7Q2CD-128", true},
		{"ZZZZZZZZZZ-18446744073709551615", true},
		{"9K3MT7Q2CD-0", false},
		{"9K3MT7Q2CD-01", false},
		{"9K3MT7Q2CI-1", false},
		{"9k3MT7Q2CD-1", false},
		{"9K3MT7Q2CD-18446744073709551616", false},
		{"srv-legacy", false},
	}
	for _, test := range tests {
		if actual := validLineID(test.value); actual != test.valid {
			t.Errorf("validLineID(%q) = %t, want %t", test.value, actual, test.valid)
		}
	}
}

func TestMintServerLineIDIsCanonicalAndUnused(t *testing.T) {
	value := &room{tombstones: make(map[string]protocol.Line)}
	lineID, err := mintServerLineID(value)
	if err != nil {
		t.Fatal(err)
	}
	if !validLineID(lineID) || lineExists(value, lineID) {
		t.Fatalf("minted invalid or used ID %q", lineID)
	}
}
