' EXPECT_OUT: RESULT: ok
' COVER: Zanna.Network.Http.Get
' COVER: Zanna.Network.Http.GetBytes
' COVER: Zanna.Network.Http.Post
' COVER: Zanna.Network.Http.PostBytes
' COVER: Zanna.Network.Http.Download
' COVER: Zanna.Network.Http.Head
' COVER: Zanna.Network.HttpReq.New
' COVER: Zanna.Network.HttpReq.Send
' COVER: Zanna.Network.HttpReq.SetBody
' COVER: Zanna.Network.HttpReq.SetBodyStr
' COVER: Zanna.Network.HttpReq.SetHeader
' COVER: Zanna.Network.HttpReq.SetTimeout
' COVER: Zanna.Network.HttpRes.Headers
' COVER: Zanna.Network.HttpRes.Status
' COVER: Zanna.Network.HttpRes.StatusText
' COVER: Zanna.Network.HttpRes.Body
' COVER: Zanna.Network.HttpRes.BodyStr
' COVER: Zanna.Network.HttpRes.Header
' COVER: Zanna.Network.HttpRes.IsOk
' COVER: Zanna.Network.Url.Authority
' COVER: Zanna.Network.Url.Fragment
' COVER: Zanna.Network.Url.Full
' COVER: Zanna.Network.Url.Host
' COVER: Zanna.Network.Url.HostPort
' COVER: Zanna.Network.Url.Pass
' COVER: Zanna.Network.Url.Path
' COVER: Zanna.Network.Url.Port
' COVER: Zanna.Network.Url.Query
' COVER: Zanna.Network.Url.Scheme
' COVER: Zanna.Network.Url.User
' COVER: Zanna.Network.Url.Clone
' COVER: Zanna.Text.Codec.UrlDecode
' COVER: Zanna.Network.Url.DecodeQuery
' COVER: Zanna.Network.Url.RemoveQueryParam
' COVER: Zanna.Text.Codec.UrlEncode
' COVER: Zanna.Network.Url.EncodeQuery
' COVER: Zanna.Network.Url.GetQueryParam
' COVER: Zanna.Network.Url.HasQueryParam
' COVER: Zanna.Network.Url.IsValid
' COVER: Zanna.Network.Url.New
' COVER: Zanna.Network.Url.Parse
' COVER: Zanna.Network.Url.QueryMap
' COVER: Zanna.Network.Url.Resolve
' COVER: Zanna.Network.Url.SetQueryParam

DIM baseUrl AS STRING
baseUrl = "http://example.com"

DIM html AS STRING
html = Zanna.Network.Http.Get(baseUrl)
Zanna.Core.Diagnostics.Assert(html.Length > 0, "http.get")

DIM htmlBytes AS Zanna.Collections.Bytes
htmlBytes = Zanna.Network.Http.GetBytes(baseUrl)
Zanna.Core.Diagnostics.Assert(htmlBytes.Length > 0, "http.getbytes")

DIM postRes AS STRING
postRes = Zanna.Network.Http.Post(baseUrl, "name=test")
Zanna.Core.Diagnostics.Assert(postRes.Length > 0, "http.post")

DIM payload AS Zanna.Collections.Bytes
payload = Zanna.Collections.Bytes.FromStr("abc")

DIM postBytes AS Zanna.Collections.Bytes
postBytes = Zanna.Network.Http.PostBytes(baseUrl, payload)
Zanna.Core.Diagnostics.Assert(postBytes.Length > 0, "http.postbytes")

DIM tmpDir AS STRING
tmpDir = Zanna.IO.Path.Join(Zanna.System.Machine.TempDir, "zanna_http")
Zanna.IO.Dir.MakeAll(tmpDir)
DIM outPath AS STRING
outPath = Zanna.IO.Path.Join(tmpDir, "example.html")
DIM ok AS INTEGER
ok = Zanna.Network.Http.Download(baseUrl, outPath)
Zanna.Core.Diagnostics.Assert(ok <> 0, "http.download")
Zanna.Core.Diagnostics.Assert(Zanna.IO.File.SizeBytes(outPath) > 0, "http.download.size")
Zanna.IO.File.Delete(outPath)
Zanna.IO.Dir.Remove(tmpDir)

DIM headMap AS Zanna.Collections.Map
headMap = Zanna.Network.Http.Head(baseUrl)
Zanna.Core.Diagnostics.Assert(headMap.Count > 0, "http.head")

DIM headReq AS Zanna.Network.HttpReq
headReq = Zanna.Network.HttpReq.New("HEAD", baseUrl)
DIM headRes AS Zanna.Network.HttpRes
headRes = headReq.Send()
Zanna.Core.Diagnostics.Assert(headRes.Status >= 200, "http.head.status")
Zanna.Core.Diagnostics.Assert(LEN(headRes.StatusText) > 0, "http.head.statustext")
Zanna.Core.Diagnostics.Assert(headRes.IsOk(), "http.head.isok")

DIM headHeaders AS Zanna.Collections.Map
headHeaders = headRes.Headers
Zanna.Core.Diagnostics.Assert(headHeaders.Count > 0, "http.head.headers")

DIM headType AS STRING
headType = headRes.Header("content-type")
Zanna.Core.Diagnostics.Assert(LEN(headType) > 0, "http.head.header")

DIM headBody AS Zanna.Collections.Bytes
headBody = headRes.Body()
Zanna.Core.Diagnostics.Assert(headBody.Length >= 0, "http.head.body")
DIM headBodyStr AS STRING
headBodyStr = headRes.BodyStr()
Zanna.Core.Diagnostics.Assert(headBodyStr.Length >= 0, "http.head.bodystr")

DIM req AS Zanna.Network.HttpReq
req = Zanna.Network.HttpReq.New("GET", baseUrl)
req.SetHeader("Accept", "text/html")
req.SetBodyStr("hello")
req.SetBody(payload)
req.SetTimeout(5000)

DIM res AS Zanna.Network.HttpRes
res = req.Send()
Zanna.Core.Diagnostics.Assert(res.Status >= 200, "httpreq.status")
Zanna.Core.Diagnostics.Assert(LEN(res.StatusText) > 0, "httpreq.statustext")
Zanna.Core.Diagnostics.Assert(res.IsOk(), "httpreq.isok")

DIM resHeaders AS Zanna.Collections.Map
resHeaders = res.Headers
Zanna.Core.Diagnostics.Assert(resHeaders.Count > 0, "httpreq.headers")

DIM resBody AS Zanna.Collections.Bytes
resBody = res.Body()
Zanna.Core.Diagnostics.Assert(resBody.Length > 0, "httpreq.body")

DIM resBodyStr AS STRING
resBodyStr = res.BodyStr()
Zanna.Core.Diagnostics.Assert(resBodyStr.Length > 0, "httpreq.bodystr")

DIM resHeader AS STRING
resHeader = res.Header("content-type")
Zanna.Core.Diagnostics.Assert(LEN(resHeader) > 0, "httpreq.header")

DIM url AS Zanna.Network.Url
url = Zanna.Network.Url.Parse("http://user:pass@example.com:8080/path/to?foo=bar&x=1#frag")
Zanna.Core.Diagnostics.AssertEqStr(url.Scheme, "http", "url.scheme")
Zanna.Core.Diagnostics.AssertEqStr(url.User, "user", "url.user")
Zanna.Core.Diagnostics.AssertEqStr(url.Pass, "pass", "url.pass")
Zanna.Core.Diagnostics.AssertEqStr(url.Host, "example.com", "url.host")
Zanna.Core.Diagnostics.AssertEq(url.Port, 8080, "url.port")
Zanna.Core.Diagnostics.AssertEqStr(url.Path, "/path/to", "url.path")
Zanna.Core.Diagnostics.AssertEqStr(url.Query, "foo=bar&x=1", "url.query")
Zanna.Core.Diagnostics.AssertEqStr(url.Fragment, "frag", "url.fragment")
Zanna.Core.Diagnostics.Assert(LEN(url.Authority) > 0, "url.authority")
Zanna.Core.Diagnostics.Assert(LEN(url.HostPort) > 0, "url.hostport")
Zanna.Core.Diagnostics.Assert(LEN(url.Full) > 0, "url.full")

DIM clone AS Zanna.Network.Url
clone = url.Clone()
Zanna.Core.Diagnostics.AssertEqStr(clone.Full, url.Full, "url.clone")

Zanna.Core.Diagnostics.Assert(url.HasQueryParam("foo"), "url.hasparam")
Zanna.Core.Diagnostics.AssertEqStr(url.GetQueryParam("foo"), "bar", "url.getparam")
url.SetQueryParam("new", "1")
Zanna.Core.Diagnostics.Assert(url.HasQueryParam("new"), "url.setparam")
url.RemoveQueryParam("x")
Zanna.Core.Diagnostics.Assert(NOT url.HasQueryParam("x"), "url.delparam")

DIM qmap AS Zanna.Collections.Map
qmap = url.QueryMap()
Zanna.Core.Diagnostics.Assert(qmap.Count >= 2, "url.querymap")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Core.Box.ToStr(qmap.Get("foo")), "bar", "url.querymap.foo")

DIM base AS Zanna.Network.Url
base = Zanna.Network.Url.Parse("http://example.com/a/b/c")
DIM resolved AS Zanna.Network.Url
resolved = base.Resolve("d")
Zanna.Core.Diagnostics.AssertEqStr(resolved.Full, "http://example.com/a/b/d", "url.resolve")

DIM built AS Zanna.Network.Url
built = Zanna.Network.Url.New()
built.Scheme = "http"
built.Host = "example.com"
built.Path = "/docs"
built.SetQueryParam("q", "test")
Zanna.Core.Diagnostics.Assert(Zanna.String.Contains(built.Full, "http://example.com/docs"), "url.new")

DIM enc AS STRING
enc = Zanna.Text.Codec.UrlEncode("hello world!")
Zanna.Core.Diagnostics.AssertEqStr(enc, "hello%20world%21", "url.encode")

DIM dec AS STRING
dec = Zanna.Text.Codec.UrlDecode(enc)
Zanna.Core.Diagnostics.AssertEqStr(dec, "hello world!", "url.decode")

DIM params AS Zanna.Collections.Map
params = Zanna.Collections.Map.New()
params.Set("name", Zanna.Core.Box.Str("John Doe"))
params.Set("city", Zanna.Core.Box.Str("New York"))

DIM queryStr AS STRING
queryStr = Zanna.Network.Url.EncodeQuery(params)
Zanna.Core.Diagnostics.Assert(Zanna.String.Contains(queryStr, "name=John%20Doe"), "url.encodequery.name")
Zanna.Core.Diagnostics.Assert(Zanna.String.Contains(queryStr, "city=New%20York"), "url.encodequery.city")

DIM parsed AS Zanna.Collections.Map
parsed = Zanna.Network.Url.DecodeQuery("a=1&b=2")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Core.Box.ToStr(parsed.Get("a")), "1", "url.decodequery")

Zanna.Core.Diagnostics.Assert(Zanna.Network.Url.IsValid("http://example.com"), "url.isvalid")
Zanna.Core.Diagnostics.Assert(Zanna.Network.Url.IsValid("http://") = FALSE, "url.isvalid.false")

PRINT "RESULT: ok"
END
