package collab

import (
	"math/big"
	"strings"

	"github.com/WenHe233/Aegisub-together/server/internal/protocol"
)

const (
	rankAlphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	rankWidth    = 16
)

var rankMaximum = new(big.Int).Sub(new(big.Int).Exp(big.NewInt(62), big.NewInt(rankWidth), nil), big.NewInt(1))

func canonicalizePositions(lines []protocol.Line) {
	previous := big.NewInt(-1)
	for _, line := range lines {
		value, ok := decodeRank(line.PosKey)
		if !ok || value.Cmp(previous) <= 0 {
			reindexLines(lines)
			return
		}
		previous = value
	}
}

func reindexLines(lines []protocol.Line) {
	divisor := big.NewInt(int64(len(lines) + 1))
	step := new(big.Int).Div(new(big.Int).Set(rankMaximum), divisor)
	for index := range lines {
		value := new(big.Int).Mul(step, big.NewInt(int64(index+1)))
		lines[index].PosKey = encodeRank(value)
	}
}

func positionForInsert(lines []protocol.Line, index int) string {
	canonicalizePositions(lines)
	left := big.NewInt(0)
	right := new(big.Int).Set(rankMaximum)
	if index > 0 {
		left, _ = decodeRank(lines[index-1].PosKey)
	}
	if index < len(lines) {
		right, _ = decodeRank(lines[index].PosKey)
	}
	if new(big.Int).Sub(new(big.Int).Set(right), left).Cmp(big.NewInt(1)) <= 0 {
		reindexLines(lines)
		return positionForInsert(lines, index)
	}
	middle := new(big.Int).Add(left, right)
	middle.Div(middle, big.NewInt(2))
	return encodeRank(middle)
}

func insertionIndex(lines []protocol.Line, leftID, rightID *string) int {
	leftIndex, rightIndex := -1, -1
	for index, line := range lines {
		if leftID != nil && line.LineID == *leftID {
			leftIndex = index
		}
		if rightID != nil && line.LineID == *rightID {
			rightIndex = index
		}
	}
	if leftIndex >= 0 && rightIndex >= 0 && leftIndex < rightIndex {
		return rightIndex
	}
	if leftIndex >= 0 {
		return leftIndex + 1
	}
	if rightIndex >= 0 {
		return rightIndex
	}
	return len(lines)
}

func encodeRank(value *big.Int) string {
	if value.Sign() < 0 || value.Cmp(rankMaximum) > 0 {
		panic("rank outside supported range")
	}
	if value.Sign() == 0 {
		return strings.Repeat("0", rankWidth)
	}
	base := big.NewInt(62)
	current := new(big.Int).Set(value)
	digits := make([]byte, 0, rankWidth)
	for current.Sign() > 0 {
		quotient, remainder := new(big.Int), new(big.Int)
		quotient.QuoRem(current, base, remainder)
		digits = append(digits, rankAlphabet[remainder.Int64()])
		current = quotient
	}
	for len(digits) < rankWidth {
		digits = append(digits, '0')
	}
	for left, right := 0, len(digits)-1; left < right; left, right = left+1, right-1 {
		digits[left], digits[right] = digits[right], digits[left]
	}
	return string(digits)
}

func decodeRank(key string) (*big.Int, bool) {
	if len(key) != rankWidth {
		return nil, false
	}
	value := big.NewInt(0)
	base := big.NewInt(62)
	for _, character := range []byte(key) {
		index := strings.IndexByte(rankAlphabet, character)
		if index < 0 {
			return nil, false
		}
		value.Mul(value, base)
		value.Add(value, big.NewInt(int64(index)))
	}
	return value, true
}
