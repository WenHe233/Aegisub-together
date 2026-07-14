package auth

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"

	"golang.org/x/crypto/argon2"
)

const format = "$aegisub-argon2id$v=19$m=%d,t=%d,p=%d$%s$%s"

type Params struct {
	Memory      uint32
	Iterations  uint32
	Parallelism uint8
	SaltLength  uint32
	KeyLength   uint32
}

func DefaultParams() Params {
	return Params{
		Memory:      64 * 1024,
		Iterations:  3,
		Parallelism: 2,
		SaltLength:  16,
		KeyLength:   32,
	}
}

func Hash(password string, params Params) (string, error) {
	if err := params.validate(); err != nil {
		return "", err
	}
	salt := make([]byte, params.SaltLength)
	if _, err := rand.Read(salt); err != nil {
		return "", fmt.Errorf("generate password salt: %w", err)
	}
	key := argon2.IDKey([]byte(password), salt, params.Iterations, params.Memory, params.Parallelism, params.KeyLength)
	return fmt.Sprintf(format,
		params.Memory,
		params.Iterations,
		params.Parallelism,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(key),
	), nil
}

func Verify(password, encoded string) bool {
	params, salt, expected, err := parse(encoded)
	if err != nil {
		return false
	}
	actual := argon2.IDKey([]byte(password), salt, params.Iterations, params.Memory, params.Parallelism, uint32(len(expected)))
	return subtle.ConstantTimeCompare(actual, expected) == 1
}

func (params Params) validate() error {
	if params.Memory == 0 || params.Iterations == 0 || params.Parallelism == 0 || params.SaltLength < 8 || params.KeyLength < 16 {
		return errors.New("invalid Argon2id parameters")
	}
	return nil
}

func parse(encoded string) (Params, []byte, []byte, error) {
	parts := strings.Split(encoded, "$")
	if len(parts) != 6 || parts[1] != "aegisub-argon2id" || parts[2] != "v=19" {
		return Params{}, nil, nil, errors.New("invalid password hash format")
	}
	var params Params
	if _, err := fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &params.Memory, &params.Iterations, &params.Parallelism); err != nil {
		return Params{}, nil, nil, errors.New("invalid password hash parameters")
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil {
		return Params{}, nil, nil, errors.New("invalid password salt")
	}
	expected, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil {
		return Params{}, nil, nil, errors.New("invalid password key")
	}
	params.SaltLength = uint32(len(salt))
	params.KeyLength = uint32(len(expected))
	if err := params.validate(); err != nil {
		return Params{}, nil, nil, err
	}
	return params, salt, expected, nil
}
