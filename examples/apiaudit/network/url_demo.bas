' =============================================================================
' API Audit: Zanna.Network.Url (BASIC)
' =============================================================================
' Tests: Parse, New, Scheme, Host, Port, Path, Query, Fragment, User, Pass,
'        Authority, HostPort, Full, SetQueryParam, GetQueryParam,
'        HasQueryParam, DelQueryParam, Resolve, Clone, EncodeQuery, DecodeQuery,
'        IsValid
' =============================================================================

PRINT "=== API Audit: Zanna.Network.Url ==="

' --- Parse ---
PRINT "--- Parse ---"
DIM u AS OBJECT = Zanna.Network.Url.Parse("https://user:pass@example.com:8080/path/to/page?key=val&a=b#section")
PRINT "Parsed URL"

' --- Scheme ---
PRINT "--- Scheme ---"
PRINT u.Scheme

' --- Host ---
PRINT "--- Host ---"
PRINT u.Host

' --- Port ---
PRINT "--- Port ---"
PRINT u.Port

' --- Path ---
PRINT "--- Path ---"
PRINT u.Path

' --- Query ---
PRINT "--- Query ---"
PRINT u.Query

' --- Fragment ---
PRINT "--- Fragment ---"
PRINT u.Fragment

' --- User ---
PRINT "--- User ---"
PRINT u.User

' --- Pass ---
PRINT "--- Pass ---"
PRINT u.Pass

' --- Authority ---
PRINT "--- Authority ---"
PRINT u.Authority

' --- HostPort ---
PRINT "--- HostPort ---"
PRINT u.HostPort

' --- Full ---
PRINT "--- Full ---"
PRINT u.Full

' --- New (empty URL builder) ---
PRINT "--- New ---"
DIM u2 AS OBJECT = Zanna.Network.Url.New()
u2.Scheme = "https"
u2.Host = "api.example.com"
u2.Port = 443
u2.Path = "/v1/resource"
PRINT "Built URL: "; u2.Full

' --- SetQueryParam --- (use static calls: A-072 workaround for BASIC method dispatch)
PRINT "--- SetQueryParam ---"
Zanna.Network.Url.SetQueryParam(u2, "page", "1")
Zanna.Network.Url.SetQueryParam(u2, "limit", "10")
PRINT Zanna.Network.Url.get_Full(u2)

' --- GetQueryParam ---
PRINT "--- GetQueryParam ---"
PRINT "page: "; Zanna.Network.Url.GetQueryParam(u2, "page")
PRINT "limit: "; Zanna.Network.Url.GetQueryParam(u2, "limit")

' --- HasQueryParam ---
PRINT "--- HasQueryParam ---"
PRINT "Has page: "; Zanna.Network.Url.HasQueryParam(u2, "page")
PRINT "Has missing: "; Zanna.Network.Url.HasQueryParam(u2, "missing")

' --- DelQueryParam ---
PRINT "--- DelQueryParam ---"
Zanna.Network.Url.RemoveQueryParam(u2, "page")
PRINT "Has page after delete: "; Zanna.Network.Url.HasQueryParam(u2, "page")
PRINT Zanna.Network.Url.get_Full(u2)

' --- Clone ---
PRINT "--- Clone ---"
DIM u3 AS OBJECT = Zanna.Network.Url.Clone(u)
PRINT Zanna.Network.Url.get_Full(u3)

' --- Resolve ---
PRINT "--- Resolve ---"
DIM base AS OBJECT = Zanna.Network.Url.Parse("https://example.com/a/b/c")
DIM resolved AS OBJECT = Zanna.Network.Url.Resolve(base, "/x/y/z")
PRINT Zanna.Network.Url.get_Full(resolved)

' --- EncodeQuery ---
PRINT "--- EncodeQuery ---"
DIM params AS OBJECT = Zanna.Collections.Map.New()
params.SetStr("q", "hello world&foo=bar")
PRINT Zanna.Network.Url.EncodeQuery(params)

' --- DecodeQuery ---
PRINT "--- DecodeQuery ---"
DIM decoded AS OBJECT = Zanna.Network.Url.DecodeQuery("q=hello%20world%26foo%3Dbar&page=2")
PRINT decoded.Count
PRINT decoded.GetStr("q")
PRINT decoded.GetStr("page")

' --- IsValid ---
PRINT "--- IsValid ---"
PRINT "Valid URL: "; Zanna.Network.Url.IsValid("https://example.com")
PRINT "Invalid URL: "; Zanna.Network.Url.IsValid("not a url")

' --- Setters ---
PRINT "--- Setters ---"
DIM u4 AS OBJECT = Zanna.Network.Url.New()
u4.Scheme = "ftp"
u4.Host = "files.example.com"
u4.Path = "/pub/data.txt"
u4.Fragment = "top"
u4.User = "admin"
u4.Pass = "secret"
PRINT "Scheme: "; u4.Scheme
PRINT "Host: "; u4.Host
PRINT "Path: "; u4.Path
PRINT "Fragment: "; u4.Fragment
PRINT "User: "; u4.User
PRINT "Pass: "; u4.Pass
PRINT "Full: "; u4.Full

' --- Simple URL ---
PRINT "--- Simple URL ---"
DIM u5 AS OBJECT = Zanna.Network.Url.Parse("http://localhost:3000")
PRINT "Scheme: "; u5.Scheme
PRINT "Host: "; u5.Host
PRINT "Port: "; u5.Port

PRINT "=== Url Audit Complete ==="
END
