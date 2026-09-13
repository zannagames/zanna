' test_url_ratelimit.bas — Network.Url, Network.RateLimiter

' --- Url: parse ---
DIM u AS Zanna.Network.Url
u = Zanna.Network.Url.Parse("https://user:pass@example.com:8080/path/to?key=val&a=b#frag")
PRINT "scheme: "; u.Scheme
PRINT "host: "; u.Host
PRINT "port: "; u.Port
PRINT "path: "; u.Path
PRINT "query: "; u.Query
PRINT "fragment: "; u.Fragment
PRINT "user: "; u.User
PRINT "pass: "; u.Pass
PRINT "authority: "; u.Authority
PRINT "hostport: "; u.HostPort
PRINT "full: "; u.Full

' --- Url: query params ---
PRINT "has key: "; u.HasQueryParam("key")
PRINT "get key: "; u.GetQueryParam("key")
PRINT "has miss: "; u.HasQueryParam("miss")

' --- Url: static methods ---
PRINT "encode: "; Zanna.Text.Codec.UrlEncode("hello world&foo=bar")
PRINT "decode: "; Zanna.Text.Codec.UrlDecode("hello%20world%26foo%3Dbar")
PRINT "isvalid: "; Zanna.Network.Url.IsValid("https://example.com")
PRINT "isvalid bad: "; Zanna.Network.Url.IsValid("not a url")

' --- Url: simple parse ---
DIM u2 AS Zanna.Network.Url
u2 = Zanna.Network.Url.Parse("http://localhost/api/v1")
PRINT "u2 scheme: "; u2.Scheme
PRINT "u2 host: "; u2.Host
PRINT "u2 path: "; u2.Path

' --- RateLimiter ---
DIM rl AS Zanna.Network.RateLimiter
rl = Zanna.Network.RateLimiter.New(5, 10)
PRINT "rl available: "; rl.Available
PRINT "rl max: "; rl.Max
PRINT "rl try1: "; rl.TryAcquire()
PRINT "rl available after: "; rl.Available
PRINT "rl try2: "; rl.TryAcquire()
PRINT "rl try3: "; rl.TryAcquire()
PRINT "rl try4: "; rl.TryAcquire()
PRINT "rl try5: "; rl.TryAcquire()
PRINT "rl try6 (should fail): "; rl.TryAcquire()
rl.Reset()
PRINT "rl after reset: "; rl.Available

PRINT "done"
END
