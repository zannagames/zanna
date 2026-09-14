' EXPECT_OUT: RESULT: ok
' COVER: Zanna.Network.Tcp.Connect
' COVER: Zanna.Network.Tcp.ConnectFor
' COVER: Zanna.Network.Tcp.Send
' COVER: Zanna.Network.Tcp.SendStr
' COVER: Zanna.Network.Tcp.SendAll
' COVER: Zanna.Network.Tcp.Recv
' COVER: Zanna.Network.Tcp.RecvExact
' COVER: Zanna.Network.Tcp.RecvLine
' COVER: Zanna.Network.Tcp.RecvStr
' COVER: Zanna.Network.Tcp.SetRecvTimeout
' COVER: Zanna.Network.Tcp.SetSendTimeout
' COVER: Zanna.Network.Tcp.Close
' COVER: Zanna.Network.Tcp.Host
' COVER: Zanna.Network.Tcp.Port
' COVER: Zanna.Network.Tcp.LocalPort
' COVER: Zanna.Network.Tcp.IsOpen
' COVER: Zanna.Network.Tcp.Available
' COVER: Zanna.Network.TcpServer.Listen
' COVER: Zanna.Network.TcpServer.ListenAt
' COVER: Zanna.Network.TcpServer.Accept
' COVER: Zanna.Network.TcpServer.AcceptFor
' COVER: Zanna.Network.TcpServer.Close
' COVER: Zanna.Network.TcpServer.Address
' COVER: Zanna.Network.TcpServer.Port
' COVER: Zanna.Network.TcpServer.IsListening
' COVER: Zanna.Network.Udp.New
' COVER: Zanna.Network.Udp.Bind
' COVER: Zanna.Network.Udp.BindAt
' COVER: Zanna.Network.Udp.SendTo
' COVER: Zanna.Network.Udp.SendToStr
' COVER: Zanna.Network.Udp.Recv
' COVER: Zanna.Network.Udp.RecvFrom
' COVER: Zanna.Network.Udp.RecvFor
' COVER: Zanna.Network.Udp.SenderHost
' COVER: Zanna.Network.Udp.SenderPort
' COVER: Zanna.Network.Udp.SetBroadcast
' COVER: Zanna.Network.Udp.JoinGroup
' COVER: Zanna.Network.Udp.LeaveGroup
' COVER: Zanna.Network.Udp.SetRecvTimeout
' COVER: Zanna.Network.Udp.Close
' COVER: Zanna.Network.Udp.Address
' COVER: Zanna.Network.Udp.Port
' COVER: Zanna.Network.Udp.IsBound

DIM basePort AS INTEGER
basePort = 49200 + (Zanna.Time.DateTime.NowMs() MOD 1000)

DIM server1 AS Zanna.Network.TcpServer
server1 = Zanna.Network.TcpServer.ListenAt("127.0.0.1", basePort)
Zanna.Core.Diagnostics.Assert(server1.IsListening, "tcpserver.listenat")

DIM client1 AS Zanna.Network.Tcp
client1 = Zanna.Network.Tcp.ConnectFor("127.0.0.1", basePort, 1000)

DIM serverConn1 AS Zanna.Network.Tcp
serverConn1 = server1.AcceptFor(1000)
Zanna.Core.Diagnostics.AssertNotNull(serverConn1, "tcpserver.acceptfor")

serverConn1.Close()
client1.Close()
server1.Close()

DIM server2 AS Zanna.Network.TcpServer
server2 = Zanna.Network.TcpServer.Listen(basePort + 1)
Zanna.Core.Diagnostics.Assert(server2.IsListening, "tcpserver.listen")
Zanna.Core.Diagnostics.AssertEq(server2.Port, basePort + 1, "tcpserver.port")
Zanna.Core.Diagnostics.Assert(LEN(server2.Address) > 0, "tcpserver.address")

DIM client2 AS Zanna.Network.Tcp
client2 = Zanna.Network.Tcp.Connect("127.0.0.1", basePort + 1)

DIM serverConn2 AS Zanna.Network.Tcp
serverConn2 = server2.Accept()
Zanna.Core.Diagnostics.AssertNotNull(serverConn2, "tcpserver.accept")

Zanna.Core.Diagnostics.Assert(client2.IsOpen, "tcp.isopen")
Zanna.Core.Diagnostics.Assert(client2.Port = basePort + 1, "tcp.port")
Zanna.Core.Diagnostics.Assert(client2.LocalPort > 0, "tcp.localport")
Zanna.Core.Diagnostics.Assert(LEN(client2.Host) > 0, "tcp.host")

client2.SetRecvTimeout(1000)
client2.SetSendTimeout(1000)
serverConn2.SetRecvTimeout(1000)
serverConn2.SetSendTimeout(1000)

DIM payload AS Zanna.Collections.Bytes
payload = Zanna.Collections.Bytes.FromStr("ping")

DIM sent AS INTEGER
sent = client2.Send(payload)
Zanna.Core.Diagnostics.AssertEq(sent, 4, "tcp.send")

DIM avail AS INTEGER
avail = serverConn2.Available
DIM tries AS INTEGER
tries = 0
WHILE avail < 4 AND tries < 100
    Zanna.Time.Clock.Sleep(10)
    avail = serverConn2.Available
    tries = tries + 1
WEND
Zanna.Core.Diagnostics.Assert(avail >= 4, "tcp.available")

DIM recvBytes AS Zanna.Collections.Bytes
recvBytes = serverConn2.Recv(4)
Zanna.Core.Diagnostics.AssertEq(recvBytes.Length, 4, "tcp.recv")

sent = client2.Send(payload)
DIM recvExact AS Zanna.Collections.Bytes
recvExact = serverConn2.RecvExact(4)
Zanna.Core.Diagnostics.AssertEq(recvExact.Length, 4, "tcp.recvexact")

client2.SendStr("line" + CHR$(10))
DIM line AS STRING
line = serverConn2.RecvLine()
Zanna.Core.Diagnostics.AssertEqStr(line, "line", "tcp.recvline")

client2.SendStr("word")
DIM word AS STRING
word = serverConn2.RecvStr(4)
Zanna.Core.Diagnostics.AssertEqStr(word, "word", "tcp.recvstr")

serverConn2.SendAll(payload)
DIM back AS Zanna.Collections.Bytes
back = client2.RecvExact(4)
Zanna.Core.Diagnostics.AssertEq(back.Length, 4, "tcp.sendall")

client2.Close()
serverConn2.Close()
server2.Close()

DIM udpBind AS Zanna.Network.Udp
udpBind = Zanna.Network.Udp.Bind(0)
Zanna.Core.Diagnostics.Assert(udpBind.IsBound, "udp.bind")

DIM udpServer AS Zanna.Network.Udp
udpServer = Zanna.Network.Udp.BindAt("127.0.0.1", 0)
Zanna.Core.Diagnostics.Assert(udpServer.IsBound, "udp.bindat")
Zanna.Core.Diagnostics.Assert(udpServer.Port > 0, "udp.port")
Zanna.Core.Diagnostics.Assert(LEN(udpServer.Address) > 0, "udp.address")

DIM udpClient AS Zanna.Network.Udp
udpClient = Zanna.Network.Udp.New()

udpServer.SetBroadcast(1)
udpServer.SetBroadcast(0)

udpServer.JoinGroup("239.255.0.1")
udpServer.LeaveGroup("239.255.0.1")

udpServer.SetRecvTimeout(1000)

DIM udpPort AS INTEGER
udpPort = udpServer.Port

DIM msg AS STRING
msg = "ping"
udpClient.SendToStr("127.0.0.1", udpPort, msg)
DIM udpRecv1 AS Zanna.Collections.Bytes
udpRecv1 = udpServer.RecvFrom(32)
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Collections.Bytes.ToStr(udpRecv1), msg, "udp.recvfrom")

DIM senderHost AS STRING
senderHost = udpServer.SenderHost()
Zanna.Core.Diagnostics.Assert(LEN(senderHost) > 0, "udp.senderhost")
Zanna.Core.Diagnostics.Assert(udpServer.SenderPort() > 0, "udp.senderport")

DIM msgBytes AS Zanna.Collections.Bytes
msgBytes = Zanna.Collections.Bytes.FromStr("pong")
udpClient.SendTo("127.0.0.1", udpPort, msgBytes)
DIM udpRecv2 AS Zanna.Collections.Bytes
udpRecv2 = udpServer.Recv(32)
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Collections.Bytes.ToStr(udpRecv2), "pong", "udp.recv")

udpClient.SendToStr("127.0.0.1", udpPort, "data")
DIM udpRecv3 AS Zanna.Collections.Bytes
udpRecv3 = udpServer.RecvFor(32, 1000)
IF Zanna.Core.Object.RefEquals(udpRecv3, NOTHING) THEN
    udpRecv3 = udpServer.Recv(32)
END IF
Zanna.Core.Diagnostics.AssertNotNull(udpRecv3, "udp.recvfor")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Collections.Bytes.ToStr(udpRecv3), "data", "udp.recvfor")

udpClient.Close()
udpServer.Close()
udpBind.Close()

PRINT "RESULT: ok"
END
