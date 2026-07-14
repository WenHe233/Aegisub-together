package auth

import "testing"

func testParams() Params {
	return Params{Memory: 64, Iterations: 1, Parallelism: 1, SaltLength: 16, KeyLength: 32}
}

func TestHashAndVerify(t *testing.T) {
	hash, err := Hash("correct horse battery staple", testParams())
	if err != nil {
		t.Fatal(err)
	}
	if !Verify("correct horse battery staple", hash) {
		t.Fatal("correct password was rejected")
	}
	if Verify("wrong", hash) {
		t.Fatal("incorrect password was accepted")
	}
}

func TestHashUsesRandomSalt(t *testing.T) {
	first, _ := Hash("same", testParams())
	second, _ := Hash("same", testParams())
	if first == second {
		t.Fatal("password hashes reused a salt")
	}
}

func TestVerifyRejectsMalformedHash(t *testing.T) {
	if Verify("password", "not-a-hash") {
		t.Fatal("malformed password hash was accepted")
	}
}
