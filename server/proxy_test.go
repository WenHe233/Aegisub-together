package collab

import (
	"net"
	"net/http/httptest"
	"testing"
)

func testNetworks(t *testing.T, values ...string) []*net.IPNet {
	t.Helper()
	result := make([]*net.IPNet, 0, len(values))
	for _, value := range values {
		_, network, err := net.ParseCIDR(value)
		if err != nil {
			t.Fatal(err)
		}
		result = append(result, network)
	}
	return result
}

func TestForwardedIPIsIgnoredFromUntrustedPeer(t *testing.T) {
	request := httptest.NewRequest("GET", "http://example.test/v1/ws", nil)
	request.RemoteAddr = "203.0.113.7:45000"
	request.Header.Set("X-Forwarded-For", "198.51.100.9")
	if got := clientIP(request, testNetworks(t, "172.28.0.0/16")); got != "203.0.113.7" {
		t.Fatalf("untrusted peer spoofed client IP: %q", got)
	}
}

func TestTrustedProxyUsesRightmostUntrustedForwardedIP(t *testing.T) {
	request := httptest.NewRequest("GET", "http://example.test/v1/ws", nil)
	request.RemoteAddr = "172.28.0.2:45000"
	request.Header.Set("X-Forwarded-For", "198.51.100.9, 10.2.3.4, 172.28.0.3")
	trusted := testNetworks(t, "172.28.0.0/16", "10.0.0.0/8")
	if got := clientIP(request, trusted); got != "198.51.100.9" {
		t.Fatalf("wrong forwarded client IP: %q", got)
	}
}

func TestMalformedForwardingFallsBackToTrustedPeer(t *testing.T) {
	request := httptest.NewRequest("GET", "http://example.test/v1/ws", nil)
	request.RemoteAddr = "172.28.0.2:45000"
	request.Header.Set("X-Forwarded-For", "198.51.100.9, not-an-ip")
	if got := clientIP(request, testNetworks(t, "172.28.0.0/16")); got != "172.28.0.2" {
		t.Fatalf("malformed forwarding was trusted: %q", got)
	}
}

func TestInvalidTrustedProxyCIDRIsRejected(t *testing.T) {
	if _, err := New(Config{TrustedProxyCIDRs: []string{"not-a-network"}}); err == nil {
		t.Fatal("invalid trusted proxy CIDR was accepted")
	}
}
