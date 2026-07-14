package collab

import (
	"sync"
	"time"
)

type failureRecord struct {
	failures     []time.Time
	blockedUntil time.Time
}

type failureLimiter struct {
	mu       sync.Mutex
	records  map[string]failureRecord
	window   time.Duration
	limit    int
	blockFor time.Duration
	now      func() time.Time
}

func newFailureLimiter() *failureLimiter {
	return &failureLimiter{
		records:  make(map[string]failureRecord),
		window:   5 * time.Minute,
		limit:    10,
		blockFor: 15 * time.Minute,
		now:      time.Now,
	}
}

func (limiter *failureLimiter) blocked(key string) (bool, time.Duration) {
	limiter.mu.Lock()
	defer limiter.mu.Unlock()
	record := limiter.records[key]
	now := limiter.now()
	if record.blockedUntil.After(now) {
		return true, record.blockedUntil.Sub(now)
	}
	return false, 0
}

func (limiter *failureLimiter) fail(key string) {
	limiter.mu.Lock()
	defer limiter.mu.Unlock()
	now := limiter.now()
	record := limiter.records[key]
	cutoff := now.Add(-limiter.window)
	kept := record.failures[:0]
	for _, failure := range record.failures {
		if failure.After(cutoff) {
			kept = append(kept, failure)
		}
	}
	record.failures = append(kept, now)
	if len(record.failures) >= limiter.limit {
		record.blockedUntil = now.Add(limiter.blockFor)
		record.failures = nil
	}
	limiter.records[key] = record
}

func (limiter *failureLimiter) success(key string) {
	limiter.mu.Lock()
	defer limiter.mu.Unlock()
	delete(limiter.records, key)
}
