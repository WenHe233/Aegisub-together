package collab

import (
	"testing"
	"time"
)

func TestFailureLimiterBlocksAndExpires(t *testing.T) {
	limiter := newFailureLimiter()
	current := time.Unix(1000, 0)
	limiter.now = func() time.Time { return current }
	limiter.limit = 2

	limiter.fail("127.0.0.1")
	if blocked, _ := limiter.blocked("127.0.0.1"); blocked {
		t.Fatal("blocked before failure limit")
	}
	limiter.fail("127.0.0.1")
	if blocked, _ := limiter.blocked("127.0.0.1"); !blocked {
		t.Fatal("did not block at failure limit")
	}
	current = current.Add(limiter.blockFor + time.Second)
	if blocked, _ := limiter.blocked("127.0.0.1"); blocked {
		t.Fatal("block did not expire")
	}
}

func TestFailureLimiterSuccessClearsFailures(t *testing.T) {
	limiter := newFailureLimiter()
	limiter.limit = 2
	limiter.fail("127.0.0.1")
	limiter.success("127.0.0.1")
	limiter.fail("127.0.0.1")
	if blocked, _ := limiter.blocked("127.0.0.1"); blocked {
		t.Fatal("successful authentication did not reset failure count")
	}
}
