' EXPECT_OUT: RESULT: ok
' COVER: Zanna.Network.Dns.IsIp
' COVER: Zanna.Network.Dns.IsIpv4
' COVER: Zanna.Network.Dns.IsIpv6
' COVER: Zanna.Network.Dns.LocalAddrs
' COVER: Zanna.Network.Dns.LocalHost
' COVER: Zanna.Network.Dns.Resolve
' COVER: Zanna.Network.Dns.ResolveIpv4
' COVER: Zanna.Network.Dns.ResolveIpv6
' COVER: Zanna.Network.Dns.ResolveAll
' COVER: Zanna.Network.Dns.Reverse

DIM ip4 AS STRING
ip4 = "127.0.0.1"
DIM ip6 AS STRING
ip6 = "::1"

Zanna.Core.Diagnostics.Assert(Zanna.Network.Dns.IsIp(ip4), "dns.isip")
Zanna.Core.Diagnostics.Assert(Zanna.Network.Dns.IsIpv4(ip4), "dns.isipv4")
Zanna.Core.Diagnostics.Assert(Zanna.Network.Dns.IsIpv6(ip4) = FALSE, "dns.isipv6.false")
Zanna.Core.Diagnostics.Assert(Zanna.Network.Dns.IsIpv6(ip6), "dns.isipv6")

DIM host AS STRING
host = Zanna.Network.Dns.LocalHost()
Zanna.Core.Diagnostics.Assert(host <> "", "dns.localhost")

DIM addrs AS Zanna.Collections.Seq
addrs = Zanna.Network.Dns.LocalAddrs()
Zanna.Core.Diagnostics.Assert(addrs.Count >= 1, "dns.localaddrs")

DIM resolved AS STRING
resolved = Zanna.Network.Dns.Resolve("localhost")
Zanna.Core.Diagnostics.Assert(Zanna.Network.Dns.IsIpv4(resolved) OR Zanna.Network.Dns.IsIpv6(resolved), "dns.resolve")

DIM resolved4 AS STRING
resolved4 = Zanna.Network.Dns.ResolveIpv4(ip4)
Zanna.Core.Diagnostics.Assert(Zanna.Network.Dns.IsIpv4(resolved4), "dns.resolve4")

DIM resolved6 AS STRING
resolved6 = Zanna.Network.Dns.ResolveIpv6(ip6)
Zanna.Core.Diagnostics.Assert(Zanna.Network.Dns.IsIpv6(resolved6), "dns.resolve6")

DIM all AS Zanna.Collections.Seq
all = Zanna.Network.Dns.ResolveAll("localhost")
Zanna.Core.Diagnostics.Assert(all.Count >= 1, "dns.resolveall")

DIM rev AS STRING
rev = Zanna.Network.Dns.Reverse(ip4)
Zanna.Core.Diagnostics.Assert(rev <> "", "dns.reverse")

PRINT "RESULT: ok"
END
