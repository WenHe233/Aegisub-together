package collab

import (
	"crypto/rand"
	"strconv"
)

const lineIDAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

func validLineID(value string) bool {
	if len(value) < 12 || len(value) > 31 || value[10] != '-' || value[11] == '0' {
		return false
	}
	for index := 0; index < 10; index++ {
		valid := false
		for _, candidate := range []byte(lineIDAlphabet) {
			if value[index] == candidate {
				valid = true
				break
			}
		}
		if !valid {
			return false
		}
	}
	_, err := strconv.ParseUint(value[11:], 10, 64)
	return err == nil
}

func validOptionalLineID(value *string) bool {
	return value == nil || validLineID(*value)
}

func mintServerLineID(value *room) (string, error) {
	for {
		var random [7]byte
		if _, err := rand.Read(random[:]); err != nil {
			return "", err
		}
		prefixValue := uint64(0)
		for _, part := range random {
			prefixValue = prefixValue<<8 | uint64(part)
		}
		prefixValue &= (uint64(1) << 50) - 1
		prefix := make([]byte, 10)
		for index := len(prefix) - 1; index >= 0; index-- {
			prefix[index] = lineIDAlphabet[prefixValue&31]
			prefixValue >>= 5
		}
		lineID := string(prefix) + "-1"
		if !lineExists(value, lineID) {
			return lineID, nil
		}
	}
}
