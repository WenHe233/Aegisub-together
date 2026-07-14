package main

import (
	"bytes"
	"context"
	"strings"
	"testing"

	"github.com/WenHe233/Aegisub-together/server/internal/auth"
)

func TestHashPasswordReadsSecretFromStdin(t *testing.T) {
	var output bytes.Buffer
	if err := run(context.Background(), []string{"hash-password"}, strings.NewReader("correct horse battery staple\n"), &output, &bytes.Buffer{}); err != nil {
		t.Fatal(err)
	}
	hash := strings.TrimSpace(output.String())
	if !auth.Verify("correct horse battery staple", hash) || strings.Contains(hash, "correct horse") {
		t.Fatal("hash-password did not emit a usable, non-plaintext hash")
	}
}

func TestHashPasswordRejectsCommandLineSecret(t *testing.T) {
	err := run(context.Background(), []string{"hash-password", "visible-secret"}, strings.NewReader(""), &bytes.Buffer{}, &bytes.Buffer{})
	if err == nil {
		t.Fatal("command-line password was accepted")
	}
}
