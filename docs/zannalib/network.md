---
status: active
audience: public
last-verified: 2026-07-26
---

# Network

> TCP and UDP networking with HTTP/HTTPS clients, secure HTTPS/WSS servers, and DNS resolution support.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Error Handling](#error-handling)
- [Zanna.Network.Tcp](#zannanetworktcp)
- [Zanna.Network.TcpServer](#zannanetworktcpserver)
- [Zanna.Network.Udp](#zannanetworkudp)
- [Zanna.Network.Dns](#zannanetworkdns)
- [Zanna.Network.Http](#zannanetworkhttp)
- [Zanna.Network.HttpReq](#zannanetworkhttpreq)
- [Zanna.Network.HttpRes](#zannanetworkhttpres)
- [Zanna.Network.Url](#zannanetworkurl)
- [Zanna.Network.WebSocket](#zannanetworkwebsocket)
- [Zanna.Network.RestClient](#zannanetworkrestclient)
- [Zanna.Network.RetryPolicy](#zannanetworkretrypolicy)
- [Zanna.Network.RateLimiter](#zannanetworkratelimiter)
- [Zanna.Network.HttpRouter](#zannanetworkhttprouter)
- [Zanna.Network.RouteMatch](#zannanetworkroutematch)
- [Zanna.Network.HttpServer](#zannanetworkhttpserver)
- [Zanna.Network.HttpsServer](#zannanetworkhttpsserver)
- [Zanna.Network.ServerReq](#zannanetworkserverreq)
- [Zanna.Network.ServerRes](#zannanetworkserverres)
- [Zanna.Network.ConnectionPool](#zannanetworkconnectionpool)
- [Zanna.Network.Multipart](#zannanetworkmultipart)
- [Zanna.Network.NetUtils](#zannanetworknetutils)
- [Zanna.Network.WsServer](#zannanetworkwsserver)
- [Zanna.Network.WssServer](#zannanetworkwssserver)
- [Zanna.Network.SseClient](#zannanetworksseclient)
- [Zanna.Network.HttpClient](#zannanetworkhttpclient)
- [Zanna.Network.SmtpClient](#zannanetworksmtpclient)
- [Zanna.Network.AsyncSocket](#zannanetworkasyncsocket)

---

## Error Handling

Network operations raise categorized runtime errors on failure.
BASIC programs handle these with `ON ERROR GOTO`; Zia programs can
use structured error handling. Programs without error handlers receive
a descriptive error message and clean exit.

### Error Codes

| Code | Name              | Raised When                                    |
|------|-------------------|------------------------------------------------|
| 10   | ConnectionRefused | Remote host actively refused the connection    |
| 11   | HostNotFound      | Hostname could not be resolved                 |
| 12   | ConnectionReset   | Connection reset by peer (EPIPE, RST)          |
| 13   | Timeout           | Operation timed out                            |
| 14   | ConnectionClosed  | Operation attempted on a closed connection     |
| 15   | DnsError          | DNS resolution failed                          |
| 16   | InvalidUrl        | URL is malformed or unparseable                |
| 17   | TlsError          | TLS handshake or certificate failure           |
| 18   | NetworkError      | Generic network I/O failure                    |
| 19   | ProtocolError     | Protocol-level error (HTTP, WebSocket)         |

### Default Timeouts

| Operation                    | Default |
|------------------------------|---------|
| TCP Connect                  | 30 sec  |
| HTTP request                 | 30 sec  |
| WS socket operation/phase    | 30 sec  |

Timeout values must fit the runtime socket timeout range: `0` disables an explicit timeout and positive values must be no larger than `2147483647` milliseconds. Negative or overflowing timeout arguments are treated as programming errors by the typed networking APIs. An HTTP timeout is one monotonic deadline for the complete request, including name resolution, all resolved-address attempts, TLS, request and response I/O, redirects, and response transformations. The system resolver call cannot be interrupted portably, but its elapsed time consumes the budget and no connection attempt starts after expiry. WebSocket connection attempts use one monotonic deadline across DNS resolution and every resolved address, then retain the configured timeout for the opening handshake and subsequent I/O phases.

Each individual native readiness wait does preserve one monotonic deadline across interrupted
system calls. Repeated POSIX signals or WinSock interruptions cannot restart that wait's full
duration. An orderly peer shutdown is surfaced as readable EOF, while error-only readiness uses
the socket's current native error rather than a stale `errno`/WinSock value.

### Programming Errors

Passing a null handle to an operation that requires a live connection, an invalid port number, or
null payload data is a programming error rather than a network condition. Such inputs raise
ordinary runtime traps instead of one of the categorized network error codes above. Some cleanup
operations, notably `Close()`, deliberately accept null or already-closed handles as no-ops. An
active language-level trap handler can recover from a trap, but callers should validate or correct
the input rather than treat it as a transient network failure.

URL parsers for HTTP, HTTPS, WS, WSS, and SSE reject empty hosts, malformed ports, malformed IPv6 authorities, port overflow, and control-character injection. HTTP chunked framing is parsed strictly; malformed chunk-size lines and non-empty chunk terminators fail as protocol errors instead of being partially accepted. HTTP request and response `Transfer-Encoding` handling supports a single final `chunked` coding; unsupported, duplicate, trailing-comma, or non-final transfer codings are rejected rather than falling back to `Content-Length`.

The core `Tcp`, `Udp`, `Dns`, HTTP, WebSocket-client, and `AsyncSocket` connection paths reject
embedded `NUL` bytes before passing host/address text to NUL-terminated operating-system APIs;
payload methods such as `SendStr` preserve embedded `NUL` bytes as data. The convenience surfaces reject embedded-NUL inputs as well: `SmtpClient`, `NetUtils`,
`HttpRouter` (registration traps; `Match` returns no match), `SseClient.Connect`, and the
WebSocket-server constructors. WebSocket text broadcasts carry the full runtime byte length, so
embedded NUL bytes are payload data rather than terminators.

---

## Zanna.Network.Tcp

TCP client connection for sending and receiving data over a network.

**Type:** Instance class

**Constructors:**

- `Zanna.Network.Tcp.Connect(host, port)` - Connect to a remote host
- `Zanna.Network.Tcp.ConnectFor(host, port, timeoutMs)` - Connect with a timeout

### Properties

| Property    | Type    | Description                                  |
|-------------|---------|----------------------------------------------|
| `Available` | Integer | Best-effort count of bytes currently pending  |
| `Host`      | String  | Remote host name or IP address (read-only)   |
| `IsOpen`    | Boolean | True if connection is open (read-only)       |
| `LocalPort` | Integer | Local port number (read-only)                |
| `Port`      | Integer | Remote port number (read-only)               |

### Send Methods

| Method                | Returns | Description                                    |
|-----------------------|---------|------------------------------------------------|
| `Send(data)`          | Integer | Send Bytes, return number of bytes sent        |
| `SendAll(data)`       | void    | Send all bytes, block until complete           |
| `SendStr(text)`       | Integer | Send string as UTF-8 bytes, return bytes sent  |

`SendStr` uses the runtime string byte length, so embedded NUL bytes are sent instead of truncating the payload at the first NUL.

Tcp handles carry a stable managed class identity plus an initialization marker. Every native Tcp
method validates both the class and the complete private payload before reading socket state, so a
wrong-class, undersized, or uninitialized opaque value traps instead of being treated as a file
descriptor. `Send` and `SendAll` likewise require an actual `Bytes` value. Raw send lengths and
receive sizes must be non-negative; zero remains a successful empty operation.

### Receive Methods

| Method             | Returns | Description                                          |
|--------------------|---------|------------------------------------------------------|
| `Recv(maxBytes)`   | Bytes   | Receive up to maxBytes (may return fewer)            |
| `RecvExact(count)` | Bytes   | Receive exactly count bytes, block until complete    |
| `RecvLine()`       | String  | Receive until newline (LF or CRLF), strip newline    |
| `RecvStr(maxBytes)`| String  | Receive up to maxBytes as UTF-8 string               |

`Available` uses the platform's `FIONREAD` query. A zero result means either no bytes are currently
pending or the query failed; a subsequent receive can still block. `Recv` and `RecvStr` return an
empty value both when the peer closes and when a persistent receive timeout expires. Peer closure
also sets `IsOpen` to false. `RecvExact` and `RecvLine` instead trap if a timeout or close prevents
them from completing; `RecvLine` has a 64 KiB limit and removes a trailing `CR` only when followed
by `LF`.

Receive-result ownership is trap-safe. If `Recv` consumes a short read and its exact-size Bytes
allocation fails, the original oversized Bytes is released before the allocation trap propagates.
Likewise, `RecvStr` releases its temporary Bytes and `RecvLine` releases its native line buffer if
managed String construction traps. The socket remains open after these result-allocation failures;
already received bytes stay consumed, matching ordinary socket side-effect semantics.
Receive failures snapshot the native socket error before releasing staging buffers, so cleanup
cannot accidentally turn a timeout into another error category (or vice versa).

### Timeout and Close Methods

| Method                  | Returns | Description                               |
|-------------------------|---------|-------------------------------------------|
| `Close()`               | void    | Close the connection                      |
| `SetRecvTimeout(ms)`    | void    | Set receive timeout (0 = no timeout)      |
| `SetSendTimeout(ms)`    | void    | Set send timeout (0 = no timeout)         |

Timeout updates are transactional: the stored timeout changes only after the operating system
accepts the socket option. Applying a timeout to a closed connection, or an OS option failure,
traps instead of reporting a value that was never installed.

### Connection Options

The implementation enables the following socket options by default:

- **TCP_NODELAY:** Disables Nagle's algorithm for low-latency communication

### Zia Example

> Tcp requires an active network connection and is typically used in interactive programs. Use `bind Zanna.Network.Tcp as Tcp;` and call `Tcp.Connect(host, port)`. Properties use get_ pattern: `conn.get_Host()`, `conn.get_IsOpen()`.

### BASIC Example

```basic
' Connect to a server
DIM conn AS Zanna.Network.Tcp = Zanna.Network.Tcp.Connect("example.com", 80)

' Check connection
IF conn.IsOpen THEN
    DIM remoteHost AS STRING = conn.Host
    PRINT "Connected to "; remoteHost; ":"; conn.Port
    PRINT "Local port: "; conn.LocalPort
END IF

' Send HTTP request
DIM request AS STRING = "GET / HTTP/1.1" + CHR$(13) + CHR$(10) + _
                        "Host: example.com" + CHR$(13) + CHR$(10) + _
                        CHR$(13) + CHR$(10)
conn.SendStr(request)

' Receive response
DIM response AS STRING = conn.RecvStr(4096)
PRINT response

' Close connection
conn.Close()
```

### Line-Based Protocol Example

```basic
' Connect to a line-based server (e.g., SMTP, POP3)
DIM conn AS Zanna.Network.Tcp = Zanna.Network.Tcp.Connect("mail.example.com", 25)

' Read greeting
DIM greeting AS STRING = conn.RecvLine()
PRINT "Server: "; greeting

' Send HELO command
conn.SendStr("HELO localhost" + CHR$(13) + CHR$(10))

' Read response
DIM response AS STRING = conn.RecvLine()
PRINT "Response: "; response

conn.Close()
```

### Binary Data Example

```basic
' Connect to a binary protocol server
DIM conn AS Zanna.Network.Tcp = Zanna.Network.Tcp.Connect("192.168.1.100", 9000)

' Send binary packet
DIM packet AS Zanna.Collections.Bytes = Zanna.Collections.Bytes.New(8)
packet.Set(0, 1)  ' Message type
packet.Set(1, 0)  ' Flags
packet.Set(2, 0)  ' Length high
packet.Set(3, 4)  ' Length low
packet.Set(4, 72)  ' 'H'
packet.Set(5, 69)  ' 'E'
packet.Set(6, 76)  ' 'L'
packet.Set(7, 79)  ' 'O'

conn.SendAll(packet)

' Receive response header (exactly 4 bytes)
DIM header AS Zanna.Collections.Bytes = conn.RecvExact(4)
DIM payloadLen AS INTEGER = header.Get(2) * 256 + header.Get(3)

' Receive payload
DIM payload AS Zanna.Collections.Bytes = conn.RecvExact(payloadLen)

conn.Close()
```

### Timeout Example

```basic
' Connect with timeout
DIM conn AS Zanna.Network.Tcp = Zanna.Network.Tcp.ConnectFor("slow-server.com", 8080, 5000)

' Set receive timeout
conn.SetRecvTimeout(3000)  ' 3 seconds

' Recv returns empty Bytes if no data arrives within 3 seconds.
DIM data AS Zanna.Collections.Bytes = conn.Recv(1024)

IF data.Length = 0 AND conn.IsOpen THEN
    PRINT "Receive timed out"
END IF

conn.Close()
```

### Error Handling

Connection operations trap on errors:

- `Connect()` traps on connection refused, host not found, or network error
- `ConnectFor()` traps on timeout in addition to connection errors
- `Send()`/`SendAll()` trap if connection is closed
- `Recv()` returns empty Bytes on a configured receive timeout or orderly peer close; it traps on
  other receive errors
- `RecvExact()` traps on timeout, receive error, or close before all requested bytes arrive
- `RecvLine()` traps if connection closes before newline

To handle potential connection failures gracefully, ensure the target host is reachable before connecting.

### Use Cases

- **HTTP clients:** Make simple HTTP requests
- **Line protocols:** Interact with SMTP, POP3, IMAP, FTP control
- **Binary protocols:** Communicate with custom binary servers
- **Game networking:** Real-time game client connections
- **IoT communication:** Connect to network devices

---

## Zanna.Network.TcpServer

TCP server for accepting incoming client connections.

**Type:** Instance class

**Constructors:**

- `Zanna.Network.TcpServer.Listen(port)` - Listen on all interfaces
- `Zanna.Network.TcpServer.ListenAt(address, port)` - Listen on specific interface

### Properties

| Property      | Type    | Description                              |
|---------------|---------|------------------------------------------|
| `Address`     | String  | Bound address (read-only)                |
| `IsListening` | Boolean | True if actively listening (read-only)   |
| `Port`        | Integer | Listening port number (read-only)        |

### Methods

| Method             | Returns | Description                                      |
|--------------------|---------|--------------------------------------------------|
| `Accept()`         | Tcp     | Accept connection, block until client connects   |
| `AcceptFor(ms)`    | Tcp     | Accept with timeout, returns null on timeout     |
| `Close()`          | void    | Stop listening and close the server              |

`Listen(0)` is supported and asks the OS to assign an ephemeral port. Read the actual port back from `server.Port` after the listener is created.

TcpServer handles have their own stable managed class identity and initialization marker. Listener
properties, `Accept`/`AcceptFor`, and non-null `Close` calls validate the complete native payload
before reading it; forged or wrong-class handles trap instead of being interpreted as listener
state. `Close(NOTHING)` remains an intentional no-op.

`TcpServer` supports concurrent accept waiters and a concurrent `Close`. Internally, the listener
is non-blocking and each accept registers an in-flight descriptor lease. `Close` first publishes
the stopped state, waits for registered accept loops to leave, and only then closes the native
descriptor; this prevents a descriptor-number reuse race between readiness and `accept`. An
`Accept`/`AcceptFor` already waiting when `Close` wins returns `NOTHING` without trapping. A new
accept call made after the stopped state is visible still traps with `Err_ConnectionClosed`.
Accepted `Tcp` sockets are restored to blocking mode before publication.

Listener, outbound-connection, and accepted-connection construction are transactional. If managed
object allocation traps after a socket has been bound or connected, the native socket and any host
or address staging buffer are released before the allocation diagnostic propagates.

### Zia Example

> TcpServer is accessible via `bind Zanna.Network.TcpServer as TcpServer;`. Call `TcpServer.Listen(port)` or `TcpServer.ListenAt(addr, port)`. Properties use get_ pattern.

### BASIC Example

```basic
' Start a simple echo server on port 8080
DIM server AS Zanna.Network.TcpServer = Zanna.Network.TcpServer.Listen(8080)

PRINT "Listening on port "; server.Port

' Accept one connection
DIM client AS Zanna.Network.Tcp = server.Accept()
DIM clientHost AS STRING = client.Host
PRINT "Client connected from "; clientHost; ":"; client.Port

' Echo loop
DO WHILE client.IsOpen
    DIM data AS Zanna.Collections.Bytes = client.Recv(1024)
    IF client.Available = 0 AND data.Length = 0 THEN
        EXIT DO  ' Connection closed
    END IF
    client.SendAll(data)  ' Echo back
LOOP

PRINT "Client disconnected"
client.Close()
server.Close()
```

### Accept with Timeout Example

```basic
' Check once for a connection with a 1-second timeout.
DIM server AS Zanna.Network.TcpServer = Zanna.Network.TcpServer.Listen(9000)
DIM client AS Zanna.Network.Tcp = server.AcceptFor(1000)

IF Zanna.Core.Object.RefEquals(client, NOTHING) THEN
    PRINT "No client connected before the timeout"
ELSE
    DIM clientHost AS STRING = client.Host
    PRINT "Client connected from "; clientHost
    client.Close()
END IF

server.Close()
```

### Specific Interface Example

```basic
' Listen only on localhost (not accessible from network)
DIM server AS Zanna.Network.TcpServer = Zanna.Network.TcpServer.ListenAt("127.0.0.1", 8080)

DIM boundAddress AS STRING = server.Address
PRINT "Listening on "; boundAddress; ":"; server.Port

' ... handle connections ...

server.Close()
```

### Multi-Client Example

```basic
' Simple server handling multiple sequential clients
DIM server AS Zanna.Network.TcpServer = Zanna.Network.TcpServer.Listen(7000)

FOR i = 1 TO 10
    PRINT "Waiting for client "; i
    DIM client AS Zanna.Network.Tcp = server.Accept()

    DIM clientHost AS STRING = client.Host
    PRINT "Client "; i; " connected from "; clientHost

    ' Send greeting
    client.SendStr("Hello, client " + STR$(i) + "!" + CHR$(10))

    ' Close client
    client.Close()
NEXT i

server.Close()
```

### Error Handling

Server operations trap on errors:

- `Listen()` traps if port is already in use
- `Listen()` traps on permission denied (ports below 1024 require elevated privileges)
- `ListenAt()` traps if address is invalid
- `Accept()` traps if server is closed

The `AcceptFor()` method returns `NULL` on timeout instead of trapping, allowing graceful timeout handling.

### Platform Notes

The networking implementation uses the Berkeley sockets API:

| Platform | Notes                                         |
|----------|-----------------------------------------------|
| Unix     | Standard POSIX sockets                        |
| macOS    | Standard BSD sockets                          |
| Windows  | Winsock2 (WSAStartup called automatically)    |

### Use Cases

- **Web servers:** Accept HTTP connections
- **Game servers:** Handle multiple game clients
- **Service daemons:** Provide network services
- **Testing:** Create mock servers for testing
- **IoT hubs:** Receive data from devices

---

---

## Zanna.Network.Udp

UDP datagram socket for connectionless communication.

**Type:** Instance class

**Constructors:**

- `Zanna.Network.Udp.New()` - Create an unbound UDP socket
- `Zanna.Network.Udp.Bind(port)` - Create and bind to port on all interfaces
- `Zanna.Network.Udp.BindAt(address, port)` - Bind to a specific IPv4 or IPv6 interface

### Properties

| Property  | Type    | Description                             |
|-----------|---------|-----------------------------------------|
| `Address` | String  | Last bound address (empty before binding) |
| `IsBound` | Boolean | True if socket is bound (read-only)     |
| `Port`    | Integer | Bound port number (0 if unbound)        |

### Send Methods

| Method                       | Returns | Description                           |
|------------------------------|---------|---------------------------------------|
| `SendTo(host, port, data)`   | Integer | Send Bytes to address, return bytes sent |
| `SendToStr(host, port, text)`| Integer | Send string as UTF-8, return bytes sent  |

`SendToStr` uses the runtime string byte length, so embedded NUL bytes are included in the datagram.
Both send methods validate the complete UDP receiver and require the documented managed payload
type (`Bytes` or String) before resolving the destination. An empty payload sends a real
zero-length UDP datagram; a return value of zero therefore means a datagram was transmitted, not
that the operation was skipped. UDP sends are atomic: an unexpected partial native send traps.

### Receive Methods

| Method                     | Returns | Description                                     |
|----------------------------|---------|-------------------------------------------------|
| `Recv(maxBytes)`           | Bytes   | Receive one packet and store sender info        |
| `RecvFor(maxBytes, ms)`    | Bytes   | One-shot timed receive; null on timeout               |
| `RecvFrom(maxBytes)`       | Bytes   | Alias of `Recv`; receive and store sender info  |
| `SenderHost()`             | String  | Host of the last packet received by either receive method |
| `SenderPort()`             | Integer | Port of the last packet received by either receive method |

Receive sizes must be non-negative. A zero maximum is an immediate empty operation that consumes
no packet. If a datagram exceeds a positive receive maximum, the entire offending datagram is
consumed and a protocol trap is raised; it is never returned as a truncated prefix and cannot
remain queued to wedge later receives. Sender metadata changes only after a complete result has
been constructed successfully.

Short receives allocate an exact-size `Bytes` result. If that second allocation traps, the
oversized staging `Bytes` is released before the diagnostic propagates. The datagram remains
consumed, and the socket can receive the next datagram normally.
Native receive errors are captured before that cleanup so the reported timeout/network category
cannot be changed by allocator or registry calls.

### Options and Close

| Method                  | Returns | Description                                  |
|-------------------------|---------|----------------------------------------------|
| `Close()`               | void    | Close the socket                             |
| `JoinGroup(addr)`       | void    | Join IPv4 or IPv6 multicast group |
| `LeaveGroup(addr)`      | void    | Leave multicast group                        |
| `SetBroadcast(enable)`  | void    | Enable/disable broadcast                     |
| `SetRecvTimeout(ms)`    | void    | Set receive timeout (0 = no timeout)         |

`Close()` is idempotent. After close, `IsBound` is false and `Port` is zero, while `Address`
retains the address used for the last bind. `LeaveGroup` is best-effort: it silently does nothing
for a closed socket or invalid group string, and it does not report an OS leave failure.

UDP handles carry a stable managed class identity plus an initialization marker. Every receiver
boundary validates both before reading socket state, so wrong-class, undersized, and uninitialized
opaque values trap safely even when an embedder's trap hook returns. `New`, `Bind`, and `BindAt`
publish transactionally: a managed allocation trap after native socket creation/bind closes that
socket and frees the staged address. `SetRecvTimeout` changes stored state only after the operating
system accepts the socket option and traps when called on a closed handle.

### Zia Example

> Udp is accessible via `bind Zanna.Network.Udp as Udp;`. Call `Udp.New()` to create a socket, then `Udp.Bind(port)` or use `SendTo`/`Recv`. Properties use get_ pattern.

### BASIC Example

```basic
' Simple UDP echo client/server

' Server side (receiver)
DIM server AS Zanna.Network.Udp = Zanna.Network.Udp.Bind(9000)
PRINT "Listening for UDP on port "; server.Port

' Wait for a message
DIM data AS Zanna.Collections.Bytes = server.RecvFrom(1024)
PRINT "Received "; data.Length; " bytes from "; server.SenderHost(); ":"; server.SenderPort()

' Echo back
server.SendTo(server.SenderHost(), server.SenderPort(), data)

server.Close()
```

### Client Example

```basic
' UDP client
DIM sock AS Zanna.Network.Udp = Zanna.Network.Udp.New()

' Send message
DIM msg AS STRING = "Hello UDP!"
sock.SendToStr("127.0.0.1", 9000, msg)

' Receive response (with timeout)
sock.SetRecvTimeout(5000)  ' 5 seconds
DIM response AS Zanna.Collections.Bytes = sock.Recv(1024)

IF response.Length > 0 THEN
    PRINT "Got response: "; response.Length; " bytes"
ELSE
    PRINT "No response (timeout)"
END IF

sock.Close()
```

### Broadcast Example

```basic
' Send broadcast message (requires SetBroadcast)
DIM sock AS Zanna.Network.Udp = Zanna.Network.Udp.Bind(0)  ' Bind to any port
sock.SetBroadcast(TRUE)

' Send to broadcast address
sock.SendToStr("255.255.255.255", 9000, "Discover")

sock.Close()
```

### IPv6 Example

```basic
' IPv6 loopback UDP
DIM server AS Zanna.Network.Udp = Zanna.Network.Udp.BindAt("::1", 9000)
DIM client AS Zanna.Network.Udp = Zanna.Network.Udp.BindAt("::1", 0)

client.SendToStr("::1", 9000, "hello over ipv6")
DIM data AS Zanna.Collections.Bytes = server.RecvFrom(1024)

PRINT "Sender: "; server.SenderHost(); ":"; server.SenderPort()
PRINT "Bytes: "; data.Length

client.Close()
server.Close()
```

### Multicast Example

```basic
' Join a multicast group
DIM sock AS Zanna.Network.Udp = Zanna.Network.Udp.Bind(5000)
sock.JoinGroup("239.1.2.3")

' Receive multicast messages
DIM data AS Zanna.Collections.Bytes = sock.RecvFor(1024, 5000)
IF NOT Zanna.Core.Object.RefEquals(data, NOTHING) THEN
    PRINT "Received multicast: "; data.Length; " bytes"
END IF

sock.LeaveGroup("239.1.2.3")
sock.Close()
```

### Error Handling

UDP operations trap on errors:

- `SendTo()` traps on host not found
- `SendTo()` traps if message is too large (>65507 bytes)
- `Recv()` traps if socket is closed
- `Recv()` rejects negative sizes; zero is an immediate empty operation
- `Recv()` consumes and traps with a protocol error if the next datagram is larger than the requested receive buffer
- `JoinGroup()` traps on invalid multicast address
- `Bind()` traps if port is in use or permission denied

The persistent timeout installed by `SetRecvTimeout()` makes `Recv()` / `RecvFrom()` return empty
Bytes on expiry. That result is indistinguishable from a valid zero-length UDP datagram.
`RecvFor()` instead returns `NULL` on timeout (and uses an indefinite wait when its timeout is 0).

### Address Family Notes

- `Udp.New()` prefers a dual-stack socket when the platform supports it, so one socket can usually send to both IPv4 and IPv6 destinations.
- `Udp.Bind(port)` may bind to either `0.0.0.0` or `::` depending on the platform's preferred wildcard family. Read back `sock.Address` / `sock.Port` after binding.
- `Udp.BindAt(address, port)` supports IPv4 literals, IPv6 literals, and hostnames that resolve to a local interface.
- `SenderHost()` returns IPv4 senders in dotted-quad form and IPv6 senders in canonical IPv6 text form.
- A `Udp` object is not safe for simultaneous operations from multiple threads; synchronize access
  externally.

### UDP vs TCP

| Feature           | TCP                  | UDP                      |
|-------------------|----------------------|--------------------------|
| Connection        | Connection-oriented  | Connectionless           |
| Delivery          | Reliable and ordered while connected | Best-effort, unordered   |
| Flow control      | Yes                  | No                       |
| Overhead          | Higher               | Lower                    |
| Message boundary  | Stream (no boundary) | Preserved (datagrams)    |
| Use case          | Reliable transfer    | Low-latency, real-time   |

### Packet Size

- **Runtime maximum:** 65,507 bytes. `SendTo` and `SendToStr` enforce this IPv4-compatible cap
  for both address families.
- **Common Ethernet payload:** 1,472 bytes for IPv4 or 1,452 bytes for IPv6 when the path MTU is
  1,500 bytes and no extra headers are present.
- **Conservative small datagram:** 512 bytes is often convenient, but it is not a runtime or
  end-to-end no-fragmentation guarantee.

The actual safe size depends on the path MTU and encapsulation. Oversized datagrams may be
fragmented or rejected by the OS; losing one fragment loses the whole datagram.

### Use Cases

- **Game networking:** Low-latency updates, player positions
- **DNS queries:** Simple request/response
- **DHCP:** Network configuration
- **Streaming:** Audio/video where some loss is acceptable
- **Discovery:** Finding services on local network
- **IoT sensors:** Lightweight telemetry

---

## Zanna.Network.Dns

Static utility class for DNS resolution and IP address validation.

**Type:** Static class

### Resolution Methods

| Method                  | Returns | Description                                  |
|-------------------------|---------|----------------------------------------------|
| `Resolve(hostname)`     | String  | Resolve to the first address returned by the OS resolver |
| `ResolveIpv4(hostname)`    | String  | Resolve to first IPv4 address only           |
| `ResolveIpv6(hostname)`    | String  | Resolve to first IPv6 address only           |
| `ResolveAll(hostname)`  | Seq     | Resolve to unique IPv4/IPv6 addresses in OS order |
| `Reverse(ipAddress)`    | String  | Reverse DNS lookup, return hostname          |

### Validation Methods

| Method              | Returns | Description                                  |
|---------------------|---------|----------------------------------------------|
| `IsIp(address)`     | Boolean | Check if valid IPv4 or IPv6 address          |
| `IsIpv4(address)`   | Boolean | Check canonical dotted-decimal IPv4 syntax   |
| `IsIpv6(address)`   | Boolean | Check IPv6 syntax, including optional `%zone` |

### Local Info Methods

| Method         | Returns | Description                                  |
|----------------|---------|----------------------------------------------|
| `LocalHost()`  | String  | Get local machine hostname                   |
| `LocalAddrs()` | Seq     | Get unique active-interface addresses, including loopback |

### Zia Example

```zia
module DnsDemo;

bind Zanna.Terminal;
bind Zanna.Network.Dns as Dns;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // IP validation (no network required)
    Say("IsIP 1.2.3.4: " + Fmt.Bool(Dns.IsIp("1.2.3.4")));
    Say("IsIPv4 1.2.3.4: " + Fmt.Bool(Dns.IsIpv4("1.2.3.4")));
    Say("IsIPv6 ::1: " + Fmt.Bool(Dns.IsIpv6("::1")));
    Say("LocalHost: " + Dns.LocalHost());

    // Resolve localhost
    Say("localhost: " + Dns.Resolve("localhost"));
}
```

### BASIC Example

```basic
' Resolve a hostname
DIM ip AS STRING = Zanna.Network.Dns.Resolve("example.com")
PRINT "example.com resolves to: "; ip

' Get all addresses for a hostname
DIM addrs AS Zanna.Collections.Seq = Zanna.Network.Dns.ResolveAll("google.com")
PRINT "Google.com addresses:"
FOR i = 0 TO addrs.Count - 1
    PRINT "  "; Zanna.Collections.Seq.GetStr(addrs, i)
NEXT i
```

### IP Validation Example

```basic
' Check if input is a valid IP address
DIM input AS STRING = "192.168.1.1"

IF Zanna.Network.Dns.IsIpv4(input) THEN
    PRINT input; " is a valid IPv4 address"
ELSE IF Zanna.Network.Dns.IsIpv6(input) THEN
    PRINT input; " is a valid IPv6 address"
ELSE
    PRINT input; " is a hostname, resolving..."
    DIM resolved AS STRING = Zanna.Network.Dns.Resolve(input)
    PRINT "Resolved to: "; resolved
END IF
```

### Local Network Info Example

```basic
' Get local machine info
PRINT "Local hostname: "; Zanna.Network.Dns.LocalHost()

' Get all local IP addresses
DIM localAddrs AS Zanna.Collections.Seq = Zanna.Network.Dns.LocalAddrs()
PRINT "Local addresses:"
FOR i = 0 TO localAddrs.Count - 1
    DIM addr AS STRING = Zanna.Collections.Seq.GetStr(localAddrs, i)
    IF Zanna.Network.Dns.IsIpv4(addr) THEN
        PRINT "  IPv4: "; addr
    ELSE
        PRINT "  IPv6: "; addr
    END IF
NEXT i
```

### Reverse DNS Example

```basic
' Reverse lookup an IP address
DIM hostname AS STRING = Zanna.Network.Dns.Reverse("8.8.8.8")
PRINT "8.8.8.8 is: "; hostname
```

### Error Handling

DNS operations trap on errors:

- `Resolve()` traps if hostname not found
- `ResolveIpv4()` traps if no IPv4 address exists
- `ResolveIpv6()` traps if no IPv6 address exists
- `Reverse()` traps if reverse lookup fails
- Forward and reverse lookup methods trap on null or empty input. The validation methods instead
  return false for invalid input; `LocalHost` and `LocalAddrs` take no input.

There is no way to distinguish between a non-existent domain (NXDOMAIN), a DNS server failure (SERVFAIL), or a network timeout — all result in a trap with the same message.

> **Blocking behavior:** DNS resolution is synchronous. Its duration and retry policy come from
> the operating-system resolver and can take several seconds or longer on an unresponsive setup;
> these methods do not expose a programmatic timeout.

Validation methods (`IsIpv4`, `IsIpv6`, `IsIp`) never trap and return `False` for invalid input.

`IsIpv4()` accepts canonical dotted decimal only. Each octet must be `0` through `255`, and a
multi-digit octet cannot start with zero (`01.2.3.4` is rejected). `IsIpv6()` accepts an optional
non-empty scope identifier such as `fe80::1%en0` or `fe80::1%4`. Scope identifiers are checked
syntactically by validation; methods that use the address ask the operating system to resolve the
zone against the current interface table.

`ResolveAll()` removes exact duplicate numeric records while preserving operating-system resolver
order. `LocalAddrs()` likewise removes duplicates, enumerates only operational adapters on Windows,
and returns the POSIX interface snapshot on Unix-like systems. Numeric IPv6 results preserve their
`%zone` suffix when the platform supplies one. Local address order remains platform dependent.

The returned `ResolveAll()` and `LocalAddrs()` sequences own their String elements. Runtime native
resolver/interface snapshots are released before a managed allocation trap propagates, including
when only a partial sequence has been constructed. Native local-interface enumeration failure is
reported as an empty sequence; managed allocation failure still traps.

### Address Formats

| Type | Format | Examples |
|------|--------|----------|
| IPv4 | Canonical dotted decimal | `192.168.1.1`, `10.0.0.1`, `127.0.0.1` |
| IPv6 | Colon hex, optional scope zone | `::1`, `2001:db8::1`, `fe80::1%en0` |

### Use Cases

- **Pre-connection validation:** Validate IP addresses before connecting
- **Service discovery:** Find addresses for hostnames
- **Network diagnostics:** Get local network configuration
- **Load balancing:** Use `ResolveAll` to get multiple backend addresses
- **Logging:** Convert IPs to hostnames via `Reverse`

---

## Zanna.Network.Http

Static HTTP client utilities for simple HTTP requests.

**Type:** Static class

### Methods

| Method                         | Returns | Description                                  |
|--------------------------------|---------|----------------------------------------------|
| `Download(url, destPath)`      | Boolean | Download file to destination path            |
| `Delete(url)`                  | String  | DELETE request, return response body as string |
| `DeleteBytes(url)`             | Bytes   | DELETE request, return response body as bytes |
| `Get(url)`                     | String  | GET request, return response body as string  |
| `GetBytes(url)`                | Bytes   | GET request, return response body as bytes   |
| `Head(url)`                    | Map     | HEAD request, return headers as Map          |
| `Options(url)`                 | String  | OPTIONS request, return response body as string |
| `Patch(url, body)`             | String  | PATCH with a string body, return response body |
| `Post(url, body)`              | String  | POST request with string body (`Content-Type: text/plain; charset=utf-8`) |
| `PostBytes(url, body)`         | Bytes   | POST request with Bytes body (`Content-Type: application/octet-stream` for a non-empty body) |
| `Put(url, body)`               | String  | PUT with a string body, return response body |
| `PutBytes(url, body)`          | Bytes   | PUT with a Bytes body, return response body (`application/octet-stream` for a non-empty body) |

### Zia Example

> Http is accessible via `bind Zanna.Network.Http as Http;`. Call `Http.Get(url)`, `Http.Post(url, body)`, etc. Requires network access.

### BASIC Example

```basic
' Simple GET request
DIM html AS STRING = Zanna.Network.Http.Get("http://example.com/api/data")
PRINT html

' Download a file
DIM success AS INTEGER = Zanna.Network.Http.Download("http://example.com/file.zip", "/tmp/file.zip")
IF success THEN
    PRINT "Download complete"
END IF

' POST with JSON body (the static Http.Post helper uses text/plain, so use HttpReq
' when the server requires application/json)
DIM jsonReq AS Zanna.Network.HttpReq = _
    Zanna.Network.HttpReq.New("POST", "http://api.example.com/submit")
jsonReq.SetHeader("Content-Type", "application/json")
jsonReq.SetBodyStr("{""name"": ""test"", ""value"": 42}")
DIM jsonRes AS Zanna.Network.HttpRes = jsonReq.Send()
PRINT jsonRes.BodyStr()

' Get headers only
DIM headers AS Zanna.Collections.Map = Zanna.Network.Http.Head("http://example.com/resource")
PRINT "Content-Type: "; headers.GetStr("content-type")
PRINT "Content-Length: "; headers.GetStr("content-length")
```

### Features

- **HTTP/1.1 + HTTP/2 transport** - Cleartext requests use HTTP/1.1; HTTPS requests negotiate `h2` first and fall back to HTTP/1.1
- **HTTPS support** - TLS 1.3 with trust-chain and hostname verification, subject to the verifier limitations below, plus CertificateVerify proof-of-key-possession
- **Redirect handling** - Automatically follows 301, 302, 303, 307, 308 redirects (up to 5), including relative `Location:` targets
- **Informational responses** - Consumes interim `1xx` responses (for example `100 Continue` and `103 Early Hints`) and returns the final response
- **Content-Length** - Handles Content-Length bodies
- **Chunked encoding** - Handles a single `Transfer-Encoding: chunked` response coding and rejects unsupported transfer codings
- **Gzip decoding** - `Http`, `HttpReq`, and `HttpClient` automatically advertise
  `Accept-Encoding: gzip` and transparently decode `Content-Encoding: gzip` responses. Decoding
  is bounded during expansion by the smaller of 256 MiB and 128 times the encoded body plus
  1 MiB of compatibility slack.
- **Transactional streaming download** - `Http.Download()` writes response bytes to an exclusively
  created sibling file, synchronizes it, and atomically publishes it instead of buffering the
  entire body in memory or exposing partial destination content
- **Timeout** - One default 30-second monotonic deadline covers name resolution, all address
  attempts, TLS, request and response I/O, redirects, and response transformations.
- **Input limits** - Encoded and decoded buffered bodies are capped at 256 MiB; gzip responses also
  obey the expansion budget above. Response headers are capped at 256 KiB; chunked trailers are
  capped at 64 KiB and 64 lines.

> **Download note:** `Http.Download()` intentionally keeps `Accept-Encoding` at identity and stays
> on the HTTP/1.1 path over HTTPS so the response can remain fully streamed to disk without
> buffering and decompressing the file in memory first. It validates both String handles and
> rejects embedded NUL bytes before filesystem or network work. The completed sibling temp file is
> flushed, synchronized, and atomically installed. Replacing an existing regular file preserves
> its ordinary permission bits; special set-user, set-group, and sticky bits are never copied. A
> failure removes staged content and leaves an existing destination unchanged whenever publication
> has not completed.

### HTTPS/TLS Support

The HTTP client transparently supports HTTPS URLs using TLS 1.3:

- **Automatic upgrade** - URLs starting with `https://` automatically use TLS
- **ALPN negotiation** - HTTPS requests advertise `h2,http/1.1`; the runtime uses HTTP/2 automatically when the server selects `h2`
- **Modern encryption** - TLS 1.3 with AES-128-GCM-SHA256 or
  ChaCha20-Poly1305-SHA256 and X25519 key exchange
- **Certificate verification enabled by default** - Windows validates chains with CryptoAPI and the
  system trust store. macOS and Linux use Zanna's in-tree verifier with a PEM trust bundle found at
  standard system paths or selected with `ZANNA_TLS_CA_FILE`. The in-tree verifier builds from the
  certificates presented by the peer and does not fetch missing intermediates through AIA.
  Hostnames are verified against SubjectAltName DNS names (with RFC 6125 wildcard handling) or
  CommonName as fallback; IP literals require an IP SAN. The server's CertificateVerify signature
  over the handshake transcript is checked in-tree. Supported verifier signatures are RSA
  PKCS#1/PSS with SHA-256/384/512 and ECDSA where the issuer key is P-256. The ClientHello advertises
  ECDSA P-256 and RSA-PSS SHA-256/384/512 for CertificateVerify.
- **No revocation checking** - OCSP/CRL validation is not implemented. Setting
  `ZANNA_TLS_REQUIRE_REVOCATION` to a nonzero value makes verification fail closed rather than
  silently proceeding without revocation data.
- **SNI behavior** - DNS hostnames are sent in the TLS SNI extension. IP literals are still verified against certificate IP SANs, but they are not sent in SNI.
- **Stable session ownership** - The in-tree TLS session has a distinct managed identity and a
  nonblocking finalizer that wipes secrets and closes abandoned sockets. Native descriptors remain
  pointer-width across HTTPS, SMTP, SSE, WebSocket, and WSS handoffs, including Windows `SOCKET`.
- **Checked connection publication** - Invalid hostname, CA-path, and ALPN configuration is rejected
  before the TLS session takes socket ownership. Nonblocking mode changes and `SO_ERROR` queries must
  succeed before a connected socket is published; malformed ALPN lists with empty elements fail.
- **To disable verification for a local test only:** Use `HttpReq.AllowInsecureCertificatesForTesting()` and keep that code out of production.
- **For custom TLS:** Use `Zanna.Crypto.Tls` directly when you need lower-level TLS control, or use `HttpReq` with `SetTimeout()` for request-level timeout control.

```basic
' HTTPS works exactly like HTTP
DIM data AS STRING = Zanna.Network.Http.Get("https://api.example.com/secure")
PRINT data

' Download over HTTPS
Zanna.Network.Http.Download("https://example.com/file.zip", "/tmp/file.zip")
```

### Error Handling

Except for `Download`, HTTP helpers trap on transport and protocol errors:

- Traps on invalid URL format
- Traps on connection failure
- Traps on TLS setup / handshake failure with the underlying TLS diagnostic when available (for example hostname mismatch, certificate validation failure, or handshake protocol error)
- Traps on timeout
- Traps on too many redirects (>5)

Receiving a non-2xx HTTP status is not a transport error: body-returning helpers still return the
response body. `Http.Download()` is deliberately non-trapping and returns `False` for an invalid
URL or managed handle, embedded NUL byte, allocation trap, transport/protocol failure, non-2xx
status, or destination-file failure.

### Limitations

- **No session cookie jar in the static `Http` facade** - Use `HttpClient` when you need persistent cookies across requests
- **No auth** - Use `HttpReq` for custom headers including Authorization
- **No client certificates** - Client-side TLS certificates not supported
- **Fixed Content-Type on string bodies** - `Http.Post()`, `Http.Put()`, and `Http.Patch()` send
  `Content-Type: text/plain; charset=utf-8` for non-empty bodies. For JSON or other content types,
  use `HttpReq` with `.SetHeader("Content-Type", "application/json")`.

---

## Zanna.Network.HttpReq

HTTP request builder for advanced requests with custom headers and options.

**Type:** Instance class

**Constructor:**

- `Zanna.Network.HttpReq.New(method, url)` - Create request with method and URL

### Methods

| Method                    | Returns | Description                                  |
|---------------------------|---------|----------------------------------------------|
| `Send()`                  | HttpRes | Execute the request and return response; traps on transport/setup failure |
| `SendResult()`            | Result  | Execute the request as `Result<HttpRes>`     |
| `SetBody(data)`           | HttpReq | Set request body as Bytes (chainable)        |
| `SetBodyStr(text)`        | HttpReq | Set request body as string (chainable)       |
| `SetHeader(name, value)`  | HttpReq | Set a header, replacing any case-insensitive match (chainable) |
| `AddHeader(name, value)`  | HttpReq | Append a repeated header field without replacing (chainable) |
| `SetForceHttp1(enabled)` | HttpReq | Force HTTP/1.1 and opt out of HTTP/2 ALPN    |
| `SetKeepAlive(enabled)`   | HttpReq | Reuse pooled connections for this request (chainable) |
| `SetTimeout(ms)`          | HttpReq | Set the end-to-end request deadline           |
| `SetTlsVerify(enabled)`   | HttpReq | Enable or disable HTTPS certificate verification |
| `AllowInsecureCertificatesForTesting()` | HttpReq | Disable HTTPS verification for local test fixtures only |

> **TLS configuration:** Certificate verification is enabled by default. Production code should keep it enabled. For local self-signed test fixtures only, call `.AllowInsecureCertificatesForTesting()` before `Send()`; the older `.SetTlsVerify(false)` form remains available for compatibility but is marked unsafe in API metadata. `SetTimeout(ms)` is one monotonic deadline shared by connection attempts, TLS, request and response I/O, redirects, and response transformations. Zero disables that deadline. For raw TLS connections (without HTTP), use `Zanna.Crypto.Tls` directly.
>
> **HTTP/2 control:** HTTPS requests advertise `h2,http/1.1` by default and use HTTP/2 automatically when the server selects it. Call `.SetForceHttp1(true)` when you need HTTP/1.1-specific behavior or want to suppress HTTP/2 ALPN negotiation for a particular request.
>
> **Headers and connection reuse:** `SetHeader` replaces any existing field whose name matches
> case-insensitively, so security-sensitive fields such as `Authorization` can be reliably
> overwritten. Use `AddHeader` when you intentionally want repeated same-name fields. `SetKeepAlive(true)` enables real connection reuse on a standalone request:
> at send time the request attaches a process-wide default connection pool, so consecutive
> keep-alive requests to the same host and port reuse the same socket. Requests issued through
> `RestClient` or `HttpClient` use that client's own pool instead.

`HttpReq` has a stable managed class identity. Every instance method validates a non-null receiver
before reading its native request payload. Body, header, and connection-pool setters are
transactional: validation and replacement allocation complete before the previous value is
changed, so a caught trap leaves the request reusable with its old configuration. A String body is
copied into request-owned storage; a Bytes body is validated and copied rather than retained as a
mutable alias.

The process-wide keep-alive pool used by standalone requests is initialized with a retryable,
thread-safe state machine. Concurrent first requests share one published pool, while a transient
initialization failure resets the state so a later request can retry instead of permanently
disabling connection reuse.

`SendResult()` clears the request trap frame before constructing either result variant. A received
response—including a 4xx or 5xx response—is `Result.Ok(HttpRes)`; setup, transport, timeout,
protocol, and allocation failures become `Result.Err(String)` when the diagnostic Result can be
allocated. If Result construction itself fails, the allocation trap propagates after every partial
String, Result, and response reference has been released.

### Zia Example

> HttpReq is accessible via `bind Zanna.Network.HttpReq as HttpReq;`. Call `HttpReq.New(method, url)` to create a request, configure with `SetHeader`/`SetBody`, then call `SendResult()` for production error handling. `Send()` remains available for scripts that prefer trapping on transport failure. Requires network access.

### BASIC Example

```basic
' Custom GET request with headers
DIM req AS Zanna.Network.HttpReq = Zanna.Network.HttpReq.New("GET", "http://api.example.com/data")
req.SetHeader("Accept", "application/json")
req.SetHeader("X-API-Key", "my-secret-key")
req.SetTimeout(10000)  ' 10 seconds

DIM sendResult AS Zanna.Result = req.SendResult()
IF sendResult.IsOk THEN
    DIM res AS Zanna.Network.HttpRes = sendResult.Unwrap()
    IF res.IsOk() THEN
        PRINT "Response: "; res.BodyStr()
    ELSE
        DIM statusText AS STRING = res.StatusText
        PRINT "HTTP status: "; res.Status; " "; statusText
    END IF
ELSE
    PRINT "Transport error: "; sendResult.UnwrapErrStr()
END IF
```

### Scripting Send

```basic
DIM res AS Zanna.Network.HttpRes = req.Send()
IF res.IsOk() THEN
    PRINT "Response: "; res.BodyStr()
ELSE
    DIM statusText AS STRING = res.StatusText
    PRINT "Error: "; res.Status; " "; statusText
END IF
```

### Chainable API

```basic
' All setters return the request object for chaining
DIM res AS Zanna.Network.HttpRes = Zanna.Network.HttpReq.New("POST", "http://api.example.com/submit") _
    .SetHeader("Content-Type", "application/json") _
    .SetHeader("Authorization", "Bearer token123") _
    .SetBodyStr("{""data"": ""value""}") _
    .SetTimeout(5000) _
    .Send()
```

---

## Zanna.Network.HttpRes

HTTP response object returned by `HttpReq.Send()` or by unwrapping
`HttpReq.SendResult()`.

**Type:** Instance class

### Properties

| Property     | Type    | Description                              |
|--------------|---------|------------------------------------------|
| `Status`     | Integer | HTTP status code (e.g., 200, 404)        |
| `StatusText` | String  | HTTP status text (e.g., "OK", "Not Found") |
| `Headers`    | Map     | Response headers (lowercase keys)        |

### Methods

| Method          | Returns | Description                                  |
|-----------------|---------|----------------------------------------------|
| `Body()`        | Bytes   | Get response body as Bytes                   |
| `BodyStr()`     | String  | Get response body as UTF-8 string            |
| `Header(name)`  | String  | Get specific header value (case-insensitive) |
| `IsOk()`        | Boolean | True if status is 2xx (success)              |

`Headers` returns a fresh Map copy with lowercase keys. `Header(name)` performs a
case-insensitive lookup. `Body()` likewise returns a fresh Bytes copy; `BodyStr()` treats the body
bytes as UTF-8 and does not inspect a response charset.

`HttpRes` has a stable managed class identity. Every non-null receiver is validated before native
status, header, or body storage is accessed; a forged managed handle traps instead of being cast to
a response. The historical null-receiver defaults remain: zero status/`IsOk`, empty Strings or
Bytes, and an empty header Map. Header and body snapshots are independent caller-owned values, and
an allocation trap while constructing a snapshot releases every partial Map, Seq, boxed String,
lookup key, or Bytes object before it propagates.

### Zia Example

> HttpRes is returned by `HttpReq.Send()`. Access properties via get_ pattern: `res.get_Status()`, `res.get_StatusText()`. Call `res.BodyStr()`, `res.IsOk()`, `res.Header(name)`.

### BASIC Example

```basic
DIM res AS Zanna.Network.HttpRes = Zanna.Network.HttpReq.New("GET", "http://example.com/api").Send()

' Check status
DIM statusText AS STRING = res.StatusText
PRINT "Status: "; res.Status; " "; statusText

' Check if successful
IF res.IsOk() THEN
    ' Get body as string
    DIM body AS STRING = res.BodyStr()
    PRINT "Body: "; body

    ' Get specific header
    DIM contentType AS STRING = res.Header("content-type")
    PRINT "Content-Type: "; contentType
ELSE
    PRINT "Request failed"
END IF
```

### Headers Access

```basic
' Access all headers
DIM res AS Zanna.Network.HttpRes = Zanna.Network.HttpReq.New("GET", "http://example.com/api").Send()
DIM headers AS Zanna.Collections.Map = res.Headers
DIM keys AS Zanna.Collections.Seq = headers.Keys()

FOR i = 0 TO keys.Count - 1
    DIM key AS STRING = Zanna.Collections.Seq.GetStr(keys, i)
    PRINT key; ": "; headers.GetStr(key)
NEXT i
```

### Binary Response

```basic
' Download binary data
DIM res AS Zanna.Network.HttpRes = Zanna.Network.HttpReq.New("GET", "http://example.com/image.png").Send()

IF res.IsOk() THEN
    DIM data AS Zanna.Collections.Bytes = res.Body()
    PRINT "Downloaded "; data.Length; " bytes"

    ' Save to file
    Zanna.IO.File.WriteAllBytes("/tmp/image.png", data)
END IF
```

### HTTP Status Codes

| Range   | Meaning       | Common Codes                              |
|---------|---------------|-------------------------------------------|
| 1xx     | Informational | 100 Continue                              |
| 2xx     | Success       | 200 OK, 201 Created, 204 No Content       |
| 3xx     | Redirection   | 301 Moved, 302 Found, 304 Not Modified    |
| 4xx     | Client Error  | 400 Bad Request, 401 Unauthorized, 404 Not Found |
| 5xx     | Server Error  | 500 Internal Error, 502 Bad Gateway       |

---

## Zanna.Network.Url

URL-reference parsing and construction with RFC 3986-style resolution and percent encoding. The
parser is intentionally permissive and is not a complete RFC 3986 validator.

**Type:** Instance class

**Constructors:**

- `Zanna.Network.Url.Parse(urlString)` - Parse a URL string into components
- `Zanna.Network.Url.New()` - Create an empty URL for building

### Properties

All properties are read/write.

| Property   | Type    | Description                                      |
|------------|---------|--------------------------------------------------|
| `Fragment` | String  | Fragment (without leading #)                     |
| `Host`     | String  | Hostname or IP address                           |
| `Pass`     | String  | Password (optional)                              |
| `Path`     | String  | Path component; a manually set path need not start with `/` |
| `Port`     | Integer | Port number (0 = not specified)                  |
| `Query`    | String  | Query string (without leading ?)                 |
| `Scheme`   | String  | URL scheme (http, https, ftp, etc.)              |
| `User`     | String  | Username (optional)                              |

### Computed Properties (Read-Only)

| Property    | Type   | Description                                         |
|-------------|--------|-----------------------------------------------------|
| `Authority` | String | `user:pass@host` plus an explicitly specified port, even if default |
| `HostPort`  | String | `host:port` (port omitted if default for scheme)    |
| `Full`      | String | Complete URL string                                 |

IPv6 literal hosts are bracketed automatically when `Authority`, `HostPort`, or `Full` is composed from a manually-set bare IPv6 host such as `::1`.

### Query Parameter Methods

| Method                           | Returns | Description                        |
|----------------------------------|---------|------------------------------------|
| `RemoveQueryParam(name)`            | Url     | Remove query parameter             |
| `GetQueryParam(name)`            | String  | Get query parameter value          |
| `HasQueryParam(name)`            | Boolean | Check if parameter exists          |
| `QueryMap()`                     | Map     | Get all parameters as Map          |
| `SetQueryParam(name, value)`     | Url     | Set or update query parameter      |

### Other Methods

| Method              | Returns | Description                                  |
|---------------------|---------|----------------------------------------------|
| `Clone()`           | Url     | Create a copy of this URL                    |
| `Resolve(relative)` | Url     | Resolve relative URL against this base       |

### Static Utility Methods

| Method                | Returns | Description                              |
|-----------------------|---------|------------------------------------------|
| `DecodeQuery(query)`  | Map     | Parse query string to Map                |
| `EncodeQuery(map)`    | String  | Encode Map as query string               |
| `IsValid(urlString)`  | Boolean | Check if URL string is valid             |

`EncodeQuery` and `DecodeQuery` operate on runtime string byte lengths, so embedded NUL bytes round-trip as `%00` instead of truncating. Parsed URL strings and stored URL components reject embedded NUL bytes because URL objects keep components as NUL-terminated runtime fields. `EncodeQuery` accepts String values (raw or boxed) verbatim and formats boxed Integer, Double, and Boolean values with the runtime's canonical scalar formatting (`true`/`false` for booleans); any other object value traps with `URL.EncodeQuery: unsupported value type`. Map traversal order is unspecified, so callers must not depend on the order of encoded pairs. `DecodeQuery` and `QueryMap` collapse repeated keys to the last value; `GetQueryParam` returns an empty string for both a missing key and a present empty value, so use `HasQueryParam` when that distinction matters.

### Default Ports

| Scheme | Port |
|--------|------|
| http   | 80   |
| https  | 443  |
| ftp    | 21   |
| ssh    | 22   |
| telnet | 23   |
| smtp   | 25   |
| dns    | 53   |
| pop3   | 110  |
| imap   | 143  |
| ldap   | 389  |
| ws     | 80   |
| wss    | 443  |

### Zia Example

```zia
module UrlDemo;

bind Zanna.Terminal;
bind Zanna.Network.Url as Url;
bind Zanna.Text.Codec as Codec;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Parse a URL
    var u = Url.Parse("https://example.com:8080/path?key=value#section");
    Say("Scheme: " + u.get_Scheme());
    Say("Host: " + u.get_Host());
    Say("Port: " + Fmt.Int(u.get_Port()));
    Say("Path: " + u.get_Path());
    Say("Query: " + u.get_Query());
    Say("Fragment: " + u.get_Fragment());
    Say("Full: " + u.get_Full());

    // URL encoding/decoding
    Say("Encode: " + Codec.UrlEncode("hello world!"));
    Say("Decode: " + Codec.UrlDecode("hello%20world%21"));

    // Validation
    Say("IsValid: " + Fmt.Bool(Url.IsValid("https://example.com")));
}
```

> **Note:** Url properties use the get_/set_ pattern in Zia: `u.get_Scheme()`, `u.set_Host("new.com")`, etc.

### BASIC Example

```basic
' Parse a URL
DIM u AS Zanna.Network.Url = Zanna.Network.Url.Parse("https://example.com:8080/path?key=value#section")
DIM scheme AS STRING = u.Scheme
DIM host AS STRING = u.Host
DIM path AS STRING = u.Path
DIM query AS STRING = u.Query
DIM fragment AS STRING = u.Fragment
DIM full AS STRING = u.Full
PRINT "Scheme: "; scheme
PRINT "Host: "; host
PRINT "Port: "; u.Port
PRINT "Path: "; path
PRINT "Query: "; query
PRINT "Fragment: "; fragment
PRINT "Full: "; full

' URL encoding/decoding
PRINT "Encode: "; Zanna.Text.Codec.UrlEncode("hello world!")
PRINT "Decode: "; Zanna.Text.Codec.UrlDecode("hello%20world%21")

' Validation
PRINT "IsValid: "; Zanna.Network.Url.IsValid("https://example.com")
```

### Parsing Example

```basic
' Parse a URL with all components
DIM url AS Zanna.Network.Url = Zanna.Network.Url.Parse("https://user:pass@api.example.com:8443/path?foo=bar#section")

DIM scheme AS STRING = url.Scheme
DIM user AS STRING = url.User
DIM host AS STRING = url.Host
DIM path AS STRING = url.Path
DIM query AS STRING = url.Query
DIM fragment AS STRING = url.Fragment
DIM full AS STRING = url.Full
PRINT "Scheme: "; scheme      ' "https"
PRINT "User: "; user          ' "user"
PRINT "Host: "; host          ' "api.example.com"
PRINT "Port: "; url.Port          ' 8443
PRINT "Path: "; path          ' "/path"
PRINT "Query: "; query        ' "foo=bar"
PRINT "Fragment: "; fragment  ' "section"
PRINT "Full: "; full          ' Complete URL string
```

### Building Example

```basic
' Build a URL from scratch
DIM url AS Zanna.Network.Url = Zanna.Network.Url.New()
url.Scheme = "https"
url.Host = "api.example.com"
url.Path = "/v1/users"
url.SetQueryParam("page", "1")
url.SetQueryParam("limit", "10")

DIM full AS STRING = url.Full
PRINT full  ' Contains both page=1 and limit=10; pair order is unspecified.
```

### Query Parameter Manipulation

```basic
DIM url AS Zanna.Network.Url = Zanna.Network.Url.Parse("http://example.com/?a=1&b=2")

' Check and get parameters
IF url.HasQueryParam("a") THEN
    PRINT "a = "; url.GetQueryParam("a")
END IF

' Modify parameters
url.SetQueryParam("c", "3")
url.RemoveQueryParam("a")

' Get all parameters as Map
DIM params AS Zanna.Collections.Map = url.QueryMap()
PRINT "Parameters: "; params.Count
```

### URL Resolution

```basic
DIM base AS Zanna.Network.Url = Zanna.Network.Url.Parse("http://example.com/a/b/c")

' Resolve absolute path
DIM r1 AS Zanna.Network.Url = base.Resolve("/d/e")
DIM full1 AS STRING = r1.Full
PRINT full1  ' "http://example.com/d/e"

' Resolve relative path
DIM r2 AS Zanna.Network.Url = base.Resolve("d")
DIM full2 AS STRING = r2.Full
PRINT full2  ' "http://example.com/a/b/d"

' Resolve full URL
DIM r3 AS Zanna.Network.Url = base.Resolve("https://other.com/x")
DIM full3 AS STRING = r3.Full
PRINT full3  ' "https://other.com/x"
```

### Encoding/Decoding

```basic
' Percent-encode special characters
DIM encoded AS STRING = Zanna.Text.Codec.UrlEncode("hello world!")
PRINT encoded  ' "hello%20world%21"

' Decode percent-encoded string
DIM decoded AS STRING = Zanna.Text.Codec.UrlDecode("hello%20world%21")
PRINT decoded  ' "hello world!"

' Decode does not treat + as a space outside query strings
PRINT Zanna.Text.Codec.UrlDecode("a+b")  ' "a+b"

' Encode Map as query string
DIM params AS Zanna.Collections.Map = Zanna.Collections.Map.New()
params.Set("name", "John Doe")
params.Set("city", "New York")
DIM query AS STRING = Zanna.Network.Url.EncodeQuery(params)
PRINT query  ' Pair order is unspecified.

' Decode query string to Map
DIM parsed AS Zanna.Collections.Map = Zanna.Network.Url.DecodeQuery("a=1&b=hello+world")
PRINT parsed.GetStr("a")  ' "1"
PRINT parsed.GetStr("b")  ' "hello world"
```

`Scheme` setters validate RFC 3986 scheme syntax and lowercase the stored value. `Port` setters accept `0..65535`; values outside that range trap instead of being clamped.

`Parse` recognizes both `scheme://authority` and authority-less `scheme:` forms
(`mailto:user@example.com` yields scheme `mailto`); a `host:port` spelling with a purely numeric
remainder keeps parsing as a path reference. `Parse` applies the same character rules as
`IsValid` and the component setters — unencoded whitespace, control bytes, and backslashes trap
with `Err_InvalidUrl`. `IsValid` remains a permissive *reference* check (`IsValid("abc")` is
true because a relative path is a valid reference). When you need an absolute network URL —
scheme plus non-empty host — use `IsValidAbsolute`, which is the appropriate pre-request check
for untrusted input; the protocol-specific HTTP/WS/SSE parsers still apply their own stricter
rules at connection time.

---

## Zanna.Network.WebSocket

WebSocket client for bidirectional, full-duplex communication over a single TCP connection. Supports both text and
binary messages following RFC 6455.

**Type:** Instance class

**Constructors:**

- `Zanna.Network.WebSocket.Connect(url)` - Connect to WebSocket server
- `Zanna.Network.WebSocket.ConnectFor(url, timeoutMs)` - Connect with timeout
- `Zanna.Network.WebSocket.ConnectProtocol(url, subprotocol)` - Connect and require a specific subprotocol
- `Zanna.Network.WebSocket.ConnectForProtocol(url, timeoutMs, subprotocol)` - Timeout-aware connect with a specific subprotocol

### Properties

| Property      | Type    | Description                                        |
|---------------|---------|----------------------------------------------------|
| `CloseCode`   | Integer | Close status code (0 if not closed) (read-only)    |
| `CloseReason` | String  | Peer-supplied close reason, if a close frame was received |
| `IsOpen`      | Boolean | True if connection is open (read-only)             |
| `Subprotocol` | String  | Negotiated `Sec-WebSocket-Protocol` token, if any  |
| `Url`         | String  | WebSocket URL (ws:// or wss://) (read-only)        |

### Send Methods

| Method            | Returns | Description                                  |
|-------------------|---------|----------------------------------------------|
| `Ping()`          | void    | Send ping frame (server responds with pong)  |
| `Send(text)`      | void    | Send text message (UTF-8)                    |
| `SendBytes(data)` | void    | Send binary message (Bytes)                  |

### Receive Methods

| Method               | Returns | Description                                          |
|----------------------|---------|------------------------------------------------------|
| `Recv()`             | String  | Receive the next text or binary payload as a String |
| `RecvBytes()`        | Bytes   | Receive the next text or binary payload as Bytes    |
| `RecvBytesFor(ms)`   | Bytes   | Timed form of `RecvBytes`; null on timeout, close, or receive failure |
| `RecvFor(ms)`        | String  | Timed form of `Recv`; null on timeout, close, or receive failure |

Neither receive method filters by WebSocket opcode. Use the method that matches the representation
you want, and arrange message-type conventions with the peer. A binary payload returned by `Recv`
can contain arbitrary non-text bytes. An empty String/Bytes can be a valid empty message or a
close/protocol-error indication; inspect `IsOpen` and `CloseCode`. A timed receive with `ms = 0`
waits indefinitely rather than polling.

A timed receive consumes any frame bytes already captured with the HTTP upgrade response, and any
already-decrypted TLS bytes, before waiting on native socket readiness. This prevents a coalesced
first frame from being hidden until unrelated later network traffic arrives.

### Close Methods

| Method               | Returns | Description                                          |
|----------------------|---------|------------------------------------------------------|
| `Close()`            | void    | Send code 1000, complete the closing handshake, and release the transport |
| `CloseWith(code,msg)`| void    | Send a close frame, complete the closing handshake, and release the transport |

The close methods complete the RFC 6455 closing handshake: they send the close frame, mark
`IsOpen` false, wait a bounded interval (about one second) for the peer's close reply while
discarding any in-flight data frames, and then close the underlying TCP/TLS transport
deterministically. If the peer never replies, the transport is still closed when the bounded
wait expires, so a close call never blocks indefinitely and never leaves the socket to garbage
collection. A locally supplied `CloseWith` reason is not copied into `CloseReason`.

### URL Format

WebSocket URLs follow this format:
```text
ws://host[:port][/path]      # Unencrypted (port 80 default)
wss://host[:port][/path]     # TLS encrypted (port 443 default)
```

### Zia Example

> WebSocket is accessible via `bind Zanna.Network.WebSocket as WS;`. Call `WS.Connect(url)` or `WS.ConnectFor(url, ms)`. Properties use get_ pattern: `ws.get_Url()`, `ws.get_IsOpen()`. Requires network access.

### BASIC Example

```basic
' Connect to a WebSocket server
DIM ws AS Zanna.Network.WebSocket = Zanna.Network.WebSocket.Connect("wss://echo.websocket.org/")

IF ws.IsOpen THEN
    DIM connectedUrl AS STRING = ws.Url
    PRINT "Connected to "; connectedUrl

    ' Send a text message
    ws.Send("Hello, WebSocket!")

    ' Receive response
    DIM response AS STRING = ws.Recv()
    PRINT "Received: "; response

    ' Send binary data
    DIM data AS Zanna.Collections.Bytes = Zanna.Collections.Bytes.FromHex("deadbeef")
    ws.SendBytes(data)

    ' Receive binary response
    DIM binResponse AS Zanna.Collections.Bytes = ws.RecvBytes()
    PRINT "Binary: "; binResponse.ToHex()

    ' Clean close
    ws.Close()
END IF
```

### Receive with Timeout Example

```basic
DIM ws AS Zanna.Network.WebSocket = Zanna.Network.WebSocket.Connect("ws://example.com/stream")

' Receive with 5 second timeout
' RecvBytesFor returns NOTHING on timeout or close.
DO WHILE ws.IsOpen
    DIM msg AS Zanna.Collections.Bytes = ws.RecvBytesFor(5000)
    IF NOT Zanna.Core.Object.RefEquals(msg, NOTHING) THEN
        PRINT "Message: "; msg.ToStr()
    ELSE
        ' Timeout — send ping to keep connection alive
        ws.Ping()
    END IF
LOOP

DIM closeReason AS STRING = ws.CloseReason
PRINT "Connection closed: "; ws.CloseCode; " - "; closeReason
```

### Close Codes

| Code | Name              | Description                                    |
|------|-------------------|------------------------------------------------|
| 1000 | Normal Closure    | Normal close (default for `Close()`)           |
| 1001 | Going Away        | Server or client is shutting down              |
| 1002 | Protocol Error    | Protocol error received                        |
| 1003 | Unsupported Data  | Received data type not supported               |
| 1006 | Abnormal Closure  | Connection lost without close frame            |
| 1007 | Invalid Data      | Message data was invalid (e.g., bad UTF-8)     |
| 1008 | Policy Violation  | Message violates policy                        |
| 1009 | Message Too Big   | Message exceeds size limit                     |
| 1011 | Server Error      | Server encountered unexpected error            |

### Error Handling

WebSocket setup and send operations trap on errors:

- `Connect()` traps on an invalid URL or handshake failure; TCP connection failures from this
  wrapper are currently reported as generic `NetworkError`
- `ConnectFor()` additionally traps when a connection phase times out
- `Send()`/`SendBytes()` trap if connection is closed
- Operations on `wss://` URLs trap on TLS errors

Receive-side protocol, framing, UTF-8, and size failures normally record a close code, mark the
object closed, send the applicable best-effort close status, and release the TCP/TLS transport
before returning an empty value rather than propagating a trap. Timed receives return null for
timeout, close, or receive failure, and restore the connection's configured native/TLS timeout
before returning. `Ping()` is a no-op when already closed and does not expose a frame-send failure;
a failed ping still retires the unusable transport.

### Protocol Notes

- **Frame masking:** Client frames are automatically masked per RFC 6455
- **Stable identity and native handles:** Every non-null client receiver validates the stable runtime class and complete payload. Native sockets remain pointer-width through TCP, TLS, and close ownership transfers, including Windows `SOCKET`.
- **Frame validation:** Reserved bits and opcodes, masked server frames, fragmented control frames, non-minimal or high-bit payload-length encodings, invalid continuation order, invalid close-frame lengths, reserved close codes, and non-UTF-8 text/close reasons are rejected with deterministic teardown. Frames and reassembled messages are capped at 64 MiB.
- **Handshake formatting:** Client `Host` headers use canonical authority formatting, omitting default ports while still bracketing IPv6 literals
- **Client handshake validation:** The response must have an exact 101 status, bounded valid HTTP field syntax, no obsolete folding, exactly one accept value, and no duplicate selected subprotocol. Repeated `Upgrade` and `Connection` fields use combined token semantics; the accept value is compared exactly after optional-whitespace trimming. Headers are capped at 16 KiB and 100 fields.
- **Handshake validation:** `WsServer` / `WssServer` reject malformed `Sec-WebSocket-Key` values and invalid `Host` headers before switching protocols
- **Subprotocol negotiation:** `ConnectProtocol` / `ConnectForProtocol` require the server to echo the requested `Sec-WebSocket-Protocol` token or the handshake fails
- **Runtime string lengths:** `Send(text)` and close reasons use runtime string byte lengths, so embedded NUL bytes are not truncated. Connection URLs and subprotocol tokens reject embedded NUL bytes because they are serialized into NUL-terminated handshake fields.
- **Ping/pong:** Pong frames are handled automatically; use `Ping()` to test connectivity
- **Message fragmentation:** Outbound messages are sent as one complete frame. Incoming
  fragmented messages are reassembled, with a 64 MiB total-message limit.
- **Exact buffer ownership:** Binary sends use the validated contiguous Bytes span directly.
  Completed receives are published with one exact managed copy, and allocation failure frees the
  consumed native frame before propagating the trap.
- **UTF-8 validation:** Text messages are validated for proper UTF-8 encoding
- **Subprotocols:** Single-token negotiation is supported; multi-option client preference lists are not exposed yet
- **Resource/threading model:** WebSocket objects are not safe for concurrent calls without
  external synchronization. Incoming handshake headers are limited to 16 KiB.

### Use Cases

- **Real-time applications:** Chat, notifications, live updates
- **Gaming:** Multiplayer game state synchronization
- **Financial feeds:** Stock tickers, trading data
- **IoT:** Device monitoring and control
- **Collaborative tools:** Real-time document editing

---

## Zanna.Network.RestClient

High-level REST API client with session management for consuming REST APIs. Maintains persistent headers, authentication,
and base URL configuration across multiple requests.

**Type:** Instance class

**Constructor:**

- `Zanna.Network.RestClient.New(baseUrl)` - Create client with base URL (e.g., "https://api.example.com")

### Properties

| Property    | Type    | Description                                      |
|-------------|---------|--------------------------------------------------|
| `BaseUrl`   | String  | Base URL for all requests (read-only)            |
| `KeepAlive` | Boolean | Whether requests reuse pooled keep-alive sockets |

### Configuration Methods

| Method                         | Returns | Description                                    |
|--------------------------------|---------|------------------------------------------------|
| `ClearAuth()`                  | void    | Remove authentication header                   |
| `RemoveHeader(name)`              | void    | Remove a default header                        |
| `SetAuthBasic(username, pass)` | void    | Set HTTP Basic authentication                  |
| `SetAuthBearer(token)`         | void    | Set Bearer token authentication                |
| `SetHeader(name, value)`       | void    | Set a default header for all requests          |
| `SetPoolSize(max)`             | void    | Resize the internal keep-alive pool            |
| `SetTimeout(ms)`               | void    | Set request timeout in milliseconds            |

Default-header configuration is case-insensitive, matching HTTP semantics: `SetHeader` replaces
any existing spelling of the same name (the last call's casing and value win), `RemoveHeader`
deletes every case-insensitive match, and `ClearAuth` removes every spelling of `Authorization`.
Invalid header names or values are ignored rather than trapped.

### Transport Behavior

- `RestClient` enables transport reuse by default.
- Plain `http://` requests reuse HTTP/1.1 keep-alive sockets when the response framing makes reuse safe.
- `https://` requests negotiate HTTP/2 first; when `h2` is selected, sequential requests reuse one TLS connection as separate HTTP/2 streams. Otherwise the client falls back to the HTTP/1.1 keep-alive path.
- Set `KeepAlive = false` to force one request per socket.
- The internal pool starts at 8 entries. `SetPoolSize` normalizes nonpositive values to 1 and caps
  the effective pool at 64 entries; idle connections expire after 30 seconds.
- The configured timeout is one monotonic deadline for the complete request, including name
  resolution, all address attempts, TLS, request and response I/O, redirects, and response
  transformations.

### Raw HTTP Methods

`*Result` methods are the production-friendly path: transport/setup failures
return `Result.ErrStr`, while any received HTTP response returns
`Result.Ok(HttpRes)` so callers can inspect status, headers, and body. The
shorter methods remain available for scripts that prefer trapping on transport
failure.

| Method                    | Returns | Description                                    |
|---------------------------|---------|------------------------------------------------|
| `Delete(path)`            | HttpRes | DELETE request                                 |
| `DeleteResult(path)`      | Result  | DELETE request as `Result<HttpRes>`            |
| `Get(path)`               | HttpRes | GET request to path                            |
| `GetResult(path)`         | Result  | GET request as `Result<HttpRes>`               |
| `Head(path)`              | HttpRes | HEAD request (headers only)                    |
| `HeadResult(path)`        | Result  | HEAD request as `Result<HttpRes>`              |
| `Patch(path, body)`       | HttpRes | PATCH request with string body                 |
| `PatchResult(path, body)` | Result  | PATCH request as `Result<HttpRes>`             |
| `Post(path, body)`        | HttpRes | POST request with string body                  |
| `PostResult(path, body)`  | Result  | POST request as `Result<HttpRes>`              |
| `Put(path, body)`         | HttpRes | PUT request with string body                   |
| `PutResult(path, body)`   | Result  | PUT request as `Result<HttpRes>`               |

Raw `Post`, `Put`, and `Patch` calls with a non-empty string body automatically add
`Content-Type: application/octet-stream` when no content type was already supplied.

### JSON Convenience Methods

These helpers serialize the supplied value, set JSON `Content-Type` and `Accept` fields, and
parse a successful response body. The JSON fields replace any same-name default header
(case-insensitively) on the outgoing request, so no duplicate fields are sent.

| Method                     | Returns | Description                                          |
|----------------------------|---------|------------------------------------------------------|
| `DeleteJson(path)`         | Object  | DELETE and return a parsed JSON value                |
| `GetJson(path)`            | Object  | GET and return a parsed JSON value                   |
| `PatchJson(path, json)`    | Object  | PATCH a JSON body and return parsed JSON             |
| `PostJson(path, json)`     | Object  | POST a JSON body and return parsed JSON              |
| `PutJson(path, json)`      | Object  | PUT a JSON body and return parsed JSON               |

Any JSON value can be returned, including a Map, Seq, string, number, Boolean, or null. A non-2xx
HTTP response returns null. Transport/setup failures still trap, and malformed response JSON traps
during parsing. The mutation helpers also return null for a successful empty body.

Store the response returned by each `*Result` call rather than reading mutable
last-request state; this keeps transport failures and HTTP status handling
explicit at the call site.

### Zia Example

```zia
var api = Zanna.Network.RestClient.New("https://api.example.com");
api.SetHeader("User-Agent", "ZannaDemo/1.0");
api.SetTimeout(15000);
```

### BASIC Example

```basic
' Create a REST client
DIM api AS Zanna.Network.RestClient = Zanna.Network.RestClient.New("https://api.example.com")
DIM baseUrl AS STRING = api.BaseUrl
PRINT "BaseUrl: "; baseUrl

' Set common headers
api.SetHeader("User-Agent", "ZannaDemo/1.0")

' Set timeout
api.SetTimeout(15000)

' Set Bearer auth
api.SetAuthBearer("test-token-123")

' Clear auth
api.ClearAuth()
```

### Basic Example

```basic
' Create a REST client for an API
DIM api AS Zanna.Network.RestClient = Zanna.Network.RestClient.New("https://api.example.com")

' Set common headers
api.SetHeader("User-Agent", "MyApp/1.0")
api.SetTimeout(15000)  ' 15 seconds

' Make requests - base URL is prepended automatically
DIM result AS Zanna.Result = api.GetResult("/users")
IF result.IsOk THEN
    DIM res AS Zanna.Network.HttpRes = result.Unwrap()
    IF res.IsOk() THEN
        PRINT "Users: "; res.BodyStr()
    ELSE
        PRINT "HTTP status: "; res.Status
    END IF
ELSE
    PRINT "Transport error: "; result.UnwrapErrStr()
END IF
```

### Authentication Example

```basic
' API with Bearer token authentication
DIM api AS Zanna.Network.RestClient = Zanna.Network.RestClient.New("https://api.example.com/v1")
api.SetAuthBearer("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...")

' All requests now include Authorization header
DIM profile AS Zanna.Collections.Map = api.GetJson("/me")
IF NOT Zanna.Core.Object.RefEquals(profile, NOTHING) THEN
    PRINT "Welcome, "; profile.GetStr("name")
END IF
```

### Basic Auth Example

```basic
' API with HTTP Basic authentication
DIM api AS Zanna.Network.RestClient = Zanna.Network.RestClient.New("https://api.example.com")
api.SetAuthBasic("username", "password")

' Credentials are base64-encoded in Authorization header
DIM data AS OBJECT = api.GetJson("/protected/resource")
```

### JSON API Example

```basic
' Complete CRUD example with JSON
DIM api AS Zanna.Network.RestClient = Zanna.Network.RestClient.New("https://jsonplaceholder.typicode.com")

' CREATE - POST with JSON body
DIM newPost AS Zanna.Collections.Map = Zanna.Collections.Map.New()
newPost.SetStr("title", "My Post")
newPost.SetStr("body", "Post content here")
newPost.SetInt("userId", 1)

DIM created AS Zanna.Collections.Map = api.PostJson("/posts", newPost)
IF NOT Zanna.Core.Object.RefEquals(created, NOTHING) THEN
    PRINT "Created post ID: "; created.GetInt("id")
END IF

' READ - GET JSON
DIM post AS Zanna.Collections.Map = api.GetJson("/posts/1")
IF NOT Zanna.Core.Object.RefEquals(post, NOTHING) THEN
    PRINT "Title: "; post.GetStr("title")
END IF

' UPDATE - PUT JSON
post.SetStr("title", "Updated Title")
DIM updated AS OBJECT = api.PutJson("/posts/1", post)

' PARTIAL UPDATE - PATCH JSON
DIM patch AS Zanna.Collections.Map = Zanna.Collections.Map.New()
patch.SetStr("title", "Patched Title")
DIM patched AS OBJECT = api.PatchJson("/posts/1", patch)

' DELETE
DIM deleted AS OBJECT = api.DeleteJson("/posts/1")
```

### Error Handling Example

```basic
DIM api AS Zanna.Network.RestClient = Zanna.Network.RestClient.New("https://api.example.com")

' Make a request that might fail
DIM result AS Zanna.Result = api.GetResult("/nonexistent")

IF result.IsErr THEN
    PRINT "Transport error: "; result.UnwrapErrStr()
ELSE
    DIM res AS Zanna.Network.HttpRes = result.Unwrap()
    IF res.Status = 404 THEN
        PRINT "Resource not found"
    ELSE IF res.Status = 401 THEN
        PRINT "Authentication required"
    ELSE IF res.Status >= 500 THEN
        PRINT "Server error: "; res.Status
    ELSE IF NOT res.IsOk() THEN
        PRINT "Request failed with status: "; res.Status
    ELSE
        PRINT "Response: "; res.BodyStr()
    END IF
END IF
```

### Multiple API Sessions

```basic
' Different APIs with different authentication
DIM publicApi AS Zanna.Network.RestClient = Zanna.Network.RestClient.New("https://public-api.example.com")

DIM privateApi AS Zanna.Network.RestClient = Zanna.Network.RestClient.New("https://private-api.example.com")
privateApi.SetAuthBearer("private-api-token")

DIM legacyApi AS Zanna.Network.RestClient = Zanna.Network.RestClient.New("https://legacy.example.com")
legacyApi.SetAuthBasic("service", "password123")

' Each client maintains its own configuration
DIM pub AS OBJECT = publicApi.GetJson("/public/data")
DIM priv AS OBJECT = privateApi.GetJson("/secure/data")
DIM legacy AS OBJECT = legacyApi.GetJson("/api/v1/data")
```

### Features

- **Base URL:** The constructor validates the String handle and embedded-NUL boundary, then stores
  an independent copy. It intentionally defers URL-syntax validation until a request. Request
  paths are joined by removing trailing slashes from the base and leading slashes from the path,
  then inserting one slash; this is concatenation, not RFC URL-reference resolution.
- **Persistent headers:** Headers set with `SetHeader` are sent with every request until removed.
  Replacement/removal is case-insensitive, and `ClearAuth` removes every `Authorization` spelling.
  Updates allocate and validate before publication, so a caught allocation failure preserves the
  previous complete value.
- **Authentication:** Built-in support for Bearer tokens and HTTP Basic auth
- **Timeout:** Configurable end-to-end request deadline (default 30 seconds), shared by name
  resolution, connection attempts, TLS, request and response I/O, redirects, and response
  transformations; zero is applied to the request and disables the deadline.
- **JSON helpers:** Automatic serialization/deserialization for JSON APIs
- **Result-returning requests:** `GetResult`, `PostResult`, `PutResult`, `PatchResult`, `DeleteResult`, and `HeadResult` return `Result<HttpRes>` so transport failures and HTTP status handling stay explicit.
- **Explicit responses:** Each `*Result` call returns its own `Result<HttpRes>`; store that response rather than relying on mutable last-request state, so status and transport handling stay explicit at the call site.
- **Threading:** Base URL/default headers, timeout/pool configuration, and last-response diagnostics
  are synchronized. Each request retains a complete immutable snapshot before unlocking, so one
  `RestClient` may be shared across concurrent callers. As expected, the final `LastResponse`
  reflects whichever request publishes its response last.

> **Lifecycle note:** The RestClient owns its headers map and its reference to the last response.
> `BaseUrl()` returns an independent String copy and `LastResponse()` returns a retained response
> handle, so either value remains valid after later client mutation. JSON convenience methods
> consume their temporary raw response reference after copying/parsing the body. Runtime-managed
> resources are released when their owning handles are reclaimed.

### RestClient vs HttpReq

| Feature              | RestClient                  | HttpReq                         |
|----------------------|-----------------------------|---------------------------------|
| Base URL             | Configured once             | Full URL each request           |
| Persistent headers   | Maintained across requests  | Set per request                 |
| Authentication       | Built-in helpers            | Manual header construction      |
| JSON handling        | Automatic with *Json methods| Manual serialization            |
| Use case             | REST API consumption        | One-off or custom requests      |

---

## Zanna.Network.RetryPolicy

Configurable retry policy with backoff strategies for handling transient failures in network operations. Tracks attempt counts and computes delays between retries.

**Type:** Instance class

**Constructors:**

- `Zanna.Network.RetryPolicy.New(maxRetries, baseDelayMs)` - Fixed delay retry policy
- `Zanna.Network.RetryPolicy.Exponential(maxRetries, baseDelayMs, maxDelayMs)` - Exponential backoff

### Properties

| Property        | Type    | Description                                  |
|-----------------|---------|----------------------------------------------|
| `Attempt`       | Integer | Number of retry delays consumed so far        |
| `CanRetry`      | Boolean | True if another retry is allowed (read-only) |
| `IsExhausted`   | Boolean | True if all retries have been used           |
| `MaxRetries`    | Integer | Maximum number of retries configured         |
| `TotalAttempts` | Integer | Total number of attempts made                |

### Methods

| Method          | Signature       | Description                                              |
|-----------------|-----------------|----------------------------------------------------------|
| `NextDelay()`   | `Integer()`     | Record attempt and get delay before next retry (-1 if exhausted) |
| `Reset()`       | `Void()`        | Reset the policy for reuse                               |

### Backoff Strategies

| Strategy     | Constructor     | Delay Pattern                                    |
|--------------|-----------------|--------------------------------------------------|
| Fixed        | `New()`         | Same delay every time: `base, base, base, ...`   |
| Exponential  | `Exponential()` | Doubles each time: `base, 2*base, 4*base, ...` (capped at max, with 0-25% additive jitter) |

Negative retry counts and base delays are normalized to `0`. For exponential policies,
`maxDelayMs` values below the normalized base delay are raised to that base delay.
Additive jitter is derived from a per-policy non-cryptographic seed and the reserved attempt
number; policy construction does not request operating-system entropy. Jitter is limited by the
remaining cap headroom before it is added, so even delays near the largest `Integer` cannot
overflow.

> **Attempt count semantics:** `RetryPolicy.New(n, baseDelayMs)` allows up to `n` successful
> calls to `NextDelay()`. `Attempt` and `TotalAttempts` start at 0 and increase to 1 after the
> first call. After `n` calls the policy is exhausted; later `NextDelay()` calls return -1 without
> incrementing either property.

`RetryPolicy` is internally safe for concurrent workers. Successful concurrent `NextDelay()`
calls reserve distinct attempts atomically and collectively stop at `MaxRetries`; properties are
atomic snapshots. `Reset()` publishes attempt zero atomically and restarts that policy's
attempt-indexed jitter sequence. As with any reset, callers that need a strict phase boundary
should coordinate it with in-flight retry work.

### Zia Example

```zia
var policy = Zanna.Network.RetryPolicy.New(3, 1000);
var firstDelay = policy.NextDelay(); // 1000
```

### Example

```basic
' Fixed delay retry (3 retries, 1 second between each)
DIM policy AS Zanna.Network.RetryPolicy = Zanna.Network.RetryPolicy.New(3, 1000)

DO WHILE policy.CanRetry
    DIM delay AS INTEGER = policy.NextDelay()
    PRINT "Retry "; policy.Attempt; " waits "; delay; "ms"
LOOP

IF policy.IsExhausted THEN
    PRINT "All retries exhausted after "; policy.TotalAttempts; " attempts"
END IF
```

### Exponential Backoff Example

```basic
' Exponential backoff (5 retries, starting at 100ms, max 5 seconds)
DIM policy AS Zanna.Network.RetryPolicy = Zanna.Network.RetryPolicy.Exponential(5, 100, 5000)

' Base delays are 100, 200, 400, 800, and 1600ms. Each returned delay includes
' 0-25% additive jitter and is capped at 5000ms.
DO WHILE policy.CanRetry
    DIM delay AS INTEGER = policy.NextDelay()
    PRINT "Retry delay: "; delay; "ms"
LOOP

' Reset for reuse
policy.Reset()
```

### Use Cases

- **API calls:** Retry failed HTTP requests with backoff
- **Database connections:** Reconnect with exponential backoff
- **File operations:** Retry failed file operations
- **Message queues:** Retry message processing with delays

---

## Zanna.Network.RateLimiter

Token bucket rate limiter for controlling the rate of operations. Tokens refill continuously over time and are consumed by operations.

**Type:** Instance class
**Constructor:** `Zanna.Network.RateLimiter.New(maxTokens, refillPerSec)`

### Properties

| Property    | Type    | Description                                    |
|-------------|---------|------------------------------------------------|
| `Available` | Integer | Current available tokens (after refill)        |
| `Max`       | Integer | Maximum token capacity                         |
| `Rate`      | Double  | Token refill rate (tokens per second)          |

### Methods

| Method              | Signature       | Description                                              |
|---------------------|-----------------|----------------------------------------------------------|
| `TryAcquire()`      | `Boolean()`     | Try to consume 1 token (returns false if none available) |
| `TryAcquire(n)`    | `Boolean(Integer)` | Consume all N requested tokens or consume none        |
| `Reset()`           | `Void()`        | Reset to full capacity                                   |

### How It Works

1. The limiter starts with `maxTokens` available tokens
2. Tokens refill continuously at `refillPerSec` rate, up to `maxTokens`
3. Each operation consumes one or more tokens
4. If insufficient tokens are available, the operation is denied (returns false)

> **Token precision:** Capacity and the whole-token balance are stored as exact 64-bit integers; only the sub-token refill remainder is fractional. `Available` returns the exact whole-token count, and `TryAcquire`/`TryAcquire(n)` consume whole tokens only. Not thread-safe — external synchronization required for concurrent use.

`maxTokens <= 0` is normalized to 1. A non-finite, zero, or negative `refillPerSec` is normalized
to 1.0, and `TryAcquire(n)` returns false when `n <= 0`. Refill timing uses the runtime's shared
cross-platform monotonic microsecond clock. Any positive `Integer` capacity — including values
above 2^53 and near the 64-bit maximum — round-trips exactly through `Max`, and large
acquisitions compare exactly against the balance.

### Zia Example

```zia
var limiter = Zanna.Network.RateLimiter.New(10, 10.0);
if limiter.TryAcquire() {
    // Perform one rate-limited operation.
}
```

### Example

```basic
' Allow 10 requests per second with burst of 10
DIM limiter AS Zanna.Network.RateLimiter = Zanna.Network.RateLimiter.New(10, 10.0)

' Check before making up to 12 calls
FOR i = 1 TO 12
    IF limiter.TryAcquire() THEN
        PRINT "Request "; i; " allowed"
    ELSE
        PRINT "Request "; i; " rate limited"
    END IF
NEXT i

' Batch operations - acquire multiple tokens
IF limiter.TryAcquire(5) THEN
    PRINT "Acquired a batch of 5 tokens"
END IF

' Check available capacity
PRINT "Available: "; limiter.Available; " / "; limiter.Max
PRINT "Refill rate: "; limiter.Rate; " tokens/sec"

' Reset to full capacity
limiter.Reset()
```

### Token Bucket Algorithm

The token bucket algorithm works like a bucket that:
- Holds up to `maxTokens` tokens
- Refills at `refillPerSec` tokens per second (continuously)
- Operations take tokens from the bucket
- If the bucket is empty, operations are denied

This allows:
- **Burst capacity:** Up to `maxTokens` operations in a burst
- **Sustained rate:** `refillPerSec` operations per second on average
- **Smooth throttling:** Natural backpressure as tokens drain

### Use Cases

- **API rate limiting:** Enforce rate limits on outbound API calls
- **Request throttling:** Limit incoming request processing rate
- **Resource protection:** Prevent resource exhaustion from burst traffic
- **Shared budget:** Apply one capacity budget across multiple cooperating consumers

---

## Zanna.Network.HttpRouter

URL pattern matching with parameter extraction for HTTP routing.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.HttpRouter.New()` - Create a new router

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Add(method, pattern)` | HttpRouter | Add a route for any HTTP method |
| `Get(pattern)` | HttpRouter | Add a GET route |
| `Post(pattern)` | HttpRouter | Add a POST route |
| `Put(pattern)` | HttpRouter | Add a PUT route |
| `Delete(pattern)` | HttpRouter | Add a DELETE route |
| `Match(method, path)` | RouteMatch | Match a request; returns NULL if no match |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `Count` | Integer | Number of registered routes |

### Pattern Syntax

- `/users` — Exact match
- `/users/:id` — Parameter capture (`:id` matches one segment)
- `/static/*path` — Wildcard (captures rest of path)

Routes are tested in registration order and the first match wins. Method comparison is
case-sensitive, matching HTTP method-token semantics — the convenience helpers register the
canonical uppercase forms, and `Add` preserves whatever casing you supply. `Add` accepts custom
methods, but the method must be a non-empty HTTP token: ASCII letters, digits, or
``!#$%&'*+-.^_`|~``; whitespace and control bytes trap without registering a route. Leading,
trailing, and repeated `/` characters are deliberately normalized while parsing *patterns*, so
`/a//b/` and `/a/b` describe the same segments; request paths are matched segment-exactly aside
from leading/trailing slashes. A `:name` capture consumes exactly one non-empty request segment,
so `/users/:id` does not match `/users//`. Captures are raw path bytes; they are not URL decoded.

Registration validates the grammar: a wildcard must be the final segment
(`/a/*rest/c` traps with `HttpRouter: wildcard segment must be terminal`), and duplicate
capture names trap with `HttpRouter: duplicate capture name in pattern`. The markers `:` and `*`
must have a name; a lone marker traps instead of being treated as a literal. A wildcard capture
may be empty and has trailing slashes removed from the stored remainder.

`HttpRouter` is internally synchronized: `Add` (and the method helpers) take a write lock while
`Match` and `Count` take a shared read lock, so registering and matching routes concurrently is
safe, and concurrent matches run in parallel. The shared-lock search performs no managed
allocations; parameter extraction and result construction happen after unlock, so an allocation
trap cannot strand the router lock. Literal-only matches do not create an empty parameter Map.
Methods and patterns containing embedded NUL bytes are rejected at registration (trap), and
`Match` treats NUL-bearing inputs as no match — a route can never be registered or matched as a
truncated prefix.

> **Typing note:** The registry declares fluent router results and `Match` as unqualified `obj`.
> Direct chains can therefore infer `HttpRouter` or `Any` instead of the concrete runtime result.
> Use an explicitly typed local or an explicit receiver such as
> `Zanna.Network.RouteMatch.Param(match, "id")` until the registry signatures are corrected.

## Zanna.Network.RouteMatch

The result of a successful `HttpRouter.Match` call. A route match is produced by the router and
is not constructed directly. The match is an owned managed reference. `Param` and `Pattern`
return owned strings; callers using the C ABI must release them.

**Type:** Value object

| Property/Method | Type | Description |
|-----------------|------|-------------|
| `Param(name)` | String | Get captured parameter value |
| `Index` | Integer | Route index (registration order) |
| `Pattern` | String | Matched pattern string |

---

## Zanna.Network.HttpServer

Threaded HTTP/1.1 server with routing and handler-tag lookup.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.HttpServer.New(port)` - Create server on the given port

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Get(pattern, tag)` | void | Register a GET route with handler tag |
| `Post(pattern, tag)` | void | Register a POST route |
| `Put(pattern, tag)` | void | Register a PUT route |
| `Delete(pattern, tag)` | void | Register a DELETE route |
| `BindHandler(tag, callback)` | void | Bind a managed callback to a handler tag; normally emitted by the frontend |
| `Start()` | void | Start accepting connections in background |
| `Stop()` | void | Stop accepting and interrupt active connections |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `Port` | Integer | Listening port number |
| `IsRunning` | Boolean | True if server is accepting connections |

> **Runtime note:** Route handlers registered through the language frontends are wired through the runtime for both `HttpServer` and `HttpsServer`. Routes and bindings may be changed while stopped, including after `Stop()`, but not while running, stopping, finalizing, or executing a synchronous native dispatch. Start, Stop, route registration, and handler replacement are serialized, so concurrent lifecycle calls never publish two listeners or join one accept thread twice.

### Request Body Support

- Request bodies can be framed with either `Content-Length` or `Transfer-Encoding: chunked`.
- Request/header lines are limited to 8 KiB, a request may contain at most 100 headers, and the
  decoded request body is limited to 16 MiB.
- Only `HTTP/1.0` and `HTTP/1.1` request lines are accepted.
- Request methods must be valid HTTP tokens. Request targets must be origin-form (`/path?...`), absolute-form (`http://host/path?...` or `https://host/path?...`), or `*`; absolute-form targets are normalized to the routed path.
- Query lookups URL-decode parameter names before matching, so `%71=search` is visible as `Query("q")`.
- Query-name matching is byte-length aware after URL decoding; `q%00x` does not match `Query("q")`. Header values and request bodies preserve embedded NUL bytes when read by handlers.
- Response header names must be non-empty ASCII HTTP tokens. Names and values reject embedded NUL bytes as well as CR/LF bytes before serialization.
- `HttpServer` honors protocol-correct keep-alive semantics: HTTP/1.1 defaults to keep-alive unless `Connection: close` is present, while HTTP/1.0 requires explicit `Connection: keep-alive`. Pipelined HTTP/1.1 requests on the same socket are preserved and processed in order.
- Send and receive timeouts of 30 seconds are installed on live client sockets. They bound each
  blocking socket operation, not the total lifetime of a slowly progressing request.
- At most 4,096 connections are tracked as active. The worker pool uses the detected logical CPU
  count, clamped to 1–64 workers. It is created lazily by the first successful `Start()`, not by
  the constructor, and is reused by later stop/start cycles.
- Passing port `0` asks the operating system for an ephemeral port. Read the actual value from
  `Port` after `Start()`.
- `Start()` and `Stop()` are idempotent and safe to call concurrently. Startup failure rolls back
  a partial listener/thread transaction. `Stop()` closes the listener, interrupts every tracked
  active socket, and then waits for the accept thread and worker tasks; it does not let active
  requests finish gracefully. A handler calling `Stop()` does not wait for its own worker.
- Replacing a native handler binding publishes the new callback/context tuple while locked, then
  invokes the detached old cleanup callback after unlocking. Cleanup may therefore call back into
  server APIs without deadlocking.

> **Route registration is transactional:** the handler tag is validated (empty and embedded-NUL
> tags trap with `invalid route handler tag`) and every allocation is reserved before the route
> is added to the router, so a registration failure leaves the server fully consistent — the
> rejected pattern is not routable and the server remains safe to use after recovering from the
> trap. The same guarantee applies to `HttpsServer`.

---

## Zanna.Network.HttpsServer

Threaded TLS-backed HTTP/1.1 + HTTP/2 server built on the in-tree TLS 1.3 runtime with zero external TLS dependencies.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.HttpsServer.New(port, certFile, keyFile)` - Create an HTTPS server on the given port using PEM credentials

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Get(pattern, tag)` | void | Register a GET route with handler tag |
| `Post(pattern, tag)` | void | Register a POST route |
| `Put(pattern, tag)` | void | Register a PUT route |
| `Delete(pattern, tag)` | void | Register a DELETE route |
| `BindHandler(tag, callback)` | void | Bind a managed callback to a handler tag; normally emitted by the frontend |
| `Start()` | void | Start accepting TLS connections in background |
| `Stop()` | void | Stop accepting and interrupt active connections |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `Port` | Integer | Listening TCP port number |
| `IsRunning` | Boolean | True if the TLS listener is accepting connections |

### TLS Credential Requirements

- `certFile` must be a PEM certificate file. A leaf certificate plus any intermediate chain certificates may be concatenated into the same file.
- `keyFile` must be an unencrypted PEM private key.
- The built-in zero-dependency server path accepts either:
  - a P-256 ECDSA leaf certificate with an unencrypted SEC1 (`BEGIN EC PRIVATE KEY`) or PKCS#8 (`BEGIN PRIVATE KEY`) key
  - an RSA leaf certificate with an unencrypted PKCS#1 (`BEGIN RSA PRIVATE KEY`) or PKCS#8 (`BEGIN PRIVATE KEY`) key
- `HttpsServer` serves both HTTP/1.1 and HTTP/2 over TLS 1.3 and advertises `h2,http/1.1` via ALPN.
- When the client supplies DNS-name SNI, it is validated against the configured certificate before
  the HTTP request is accepted. Missing SNI is accepted by default; set the process environment
  variable `ZANNA_TLS_REQUIRE_SNI` to a nonzero value to reject it. IP-literal SNI is rejected.

### Runtime Notes

- `HttpsServer.New(0, certFile, keyFile)` is supported. Read the actual bound port back from `server.Port` after `Start()`.
- Both credential paths are copied by exact managed-string length. Empty paths, forged String
  handles, and embedded NUL bytes trap before native file-system access.
- Credential files are loaded when the server is constructed, are capped at 4 MiB each, and are
  not reloaded by a later stop/start cycle.
- The request/response handler model mirrors `HttpServer`.
- Sequential and pipelined HTTPS keep-alive requests on the same TLS connection are supported when response framing is safe.
- When ALPN selects `h2`, the same route table serves HTTP/2 streams through the existing `ServerReq` / `ServerRes` handler model.
- Handler-supplied HTTP/2 response field names are validated and normalized to lowercase. The
  server computes body eligibility identically for HTTP/1.1 and HTTP/2, so statuses such as 204
  suppress an attempted body and advertise the resulting exact `content-length: 0`.
- HTTP/2 request trailers are merged into the request header map and are visible through
  `req.Header(name)`; response trailers are preserved in the client-visible header list. Unknown
  HTTP/2 extension frames are ignored, informational `1xx` responses are skipped until the final
  response, and inbound frames are capped at the local advertised maximum frame size.
- If a peer opens another request stream before the active one finishes, `HttpsServer` refuses that extra stream with `RST_STREAM` and keeps the TLS connection alive for later requests.
- The built-in TLS server stack performs the full TLS 1.3 handshake in-tree, including `ClientHello`/`ServerHello`, ALPN negotiation, certificate chain delivery, `CertificateVerify`, and bidirectional `Finished` processing.
- `Start()` fails cleanly on listener-bind, worker-pool, or accept-thread startup errors instead of leaving the server in a partial running state. The pool is created lazily on first start and reused across later restarts.
- Route tables and handler bindings are immutable only while the server is running; they can be
  changed again after `Stop()`.
- The active-connection ceiling and HTTP/1.1 request limits match `HttpServer`. The HTTPS worker
  count is the detected logical CPU count clamped to 1–1,024. `Stop()` interrupts active TLS
  connections rather than gracefully draining requests.

HTTPS route registration uses the same transactional tag validation and router-publication rules
as `HttpServer`. A rejected pattern or allocation failure leaves both the router and route metadata
unchanged.

---

## Zanna.Network.ServerReq

Managed request snapshot passed to an `HttpServer` or `HttpsServer` route handler. The server
constructs one complete object for each request and releases its producer reference after response
serialization. A native or managed handler that stores the object must retain it independently;
the retained snapshot remains valid after the callback returns.

**Type:** Managed instance snapshot

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `Method` | String | Request method |
| `Path` | String | Normalized request path |
| `Body` | String | Request body |

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Header(name)` | String | Read a request header, or empty string if absent |
| `Param(name)` | String | Read a captured route parameter, or empty string if absent |
| `Query(name)` | String | Read a decoded query parameter, or empty string if absent |

`Header`, `Param`, and `Query` cannot distinguish a missing value from a present empty value.
`Query` returns the first occurrence when a key is repeated. Route captures remain URL-encoded;
query names and values are URL-decoded. `Body` preserves the request's byte length, including
embedded NUL bytes.

## Zanna.Network.ServerRes

Managed response builder passed alongside `ServerReq` to a route handler. It follows the same
producer-reference rule: a handler may retain it beyond dispatch, and its owned header Map/body
remain valid until the final retained reference is released.

**Type:** Managed instance builder

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Status(code)` | ServerRes | Set the response status and return the response builder |
| `Header(name, value)` | ServerRes | Set a response header and return the response builder |
| `Send(body)` | void | Send a string response body |
| `Json(body)` | void | Send an already-serialized JSON string with JSON content type |

`Status` accepts only 100–599 and traps outside that range. `Header` silently ignores malformed
names, CR/LF or embedded-NUL values, and server-managed fields such as `Content-Length`,
`Transfer-Encoding`, and `Connection`. Custom headers replace case-insensitively — setting
`content-type` and then `Content-Type` yields a single wire field with the last value — and
`Json` replaces any existing content-type spelling with `Content-Type: application/json`.
`Send` allocates a complete replacement body before changing the response. `Json` goes further:
it stages an exact body copy and a cloned header Map, then publishes both together. If a recovered
allocation trap interrupts either operation, the complete prior response remains visible. `Json`
does not validate that its body is well-formed JSON.

At the native C boundary, `HttpServer`, `HttpsServer`, `ServerReq`, and `ServerRes` carry distinct
stable class identities. Public entry points reject forged, stale, unrelated, undersized, or
partially initialized handles before accessing listener, mutex, request, or response payloads.

### Handler Example

```basic
DIM server AS Zanna.Network.HttpServer = Zanna.Network.HttpServer.New(8080)
server.Get("/things/:id", "HandleThing")

SUB HandleThing(req AS Zanna.Network.ServerReq, res AS Zanna.Network.ServerRes)
    DIM id AS STRING = req.Param("id")
    res.Header("Content-Type", "text/plain").Status(200).Send("Thing " + id)
END SUB
```

When a route tag is a literal function name, the BASIC and Zia frontends emit the corresponding
`BindHandler` call automatically. Register routes and handlers before `Start()`.

---

## Zanna.Network.ConnectionPool

Thread-safe plain-TCP connection pooling for reuse across HTTP requests.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.ConnectionPool.New(maxSize)` - Create pool with max connections

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Acquire(host, port)` | Tcp | Get a connection (pooled or new) |
| `Release(conn)` | void | Return connection to pool |
| `Clear()` | void | Close all pooled connections |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `Size` | Integer | Total connections in pool |
| `Available` | Integer | Idle (available) connections |

### Runtime Notes

- `ConnectionPool` pools raw TCP sockets by exact host bytes and remote port. It caches an endpoint
  hash for fast rejection, then confirms equality byte-for-byte and case-sensitively against the
  immutable endpoint already owned by the Tcp object. No per-entry key string is allocated.
- Pool hosts must be non-empty and free of embedded NUL bytes. There is no separate formatted-key
  length limit; the runtime string and operating-system resolver limits apply.
- New outbound connections are registered as in-use immediately when pool capacity allows, so `Size` and `Available` reflect checked-out state instead of only idle state.
- `maxSize` is clamped to `[1, 128]`. Acquires beyond tracked capacity still succeed, but those overflow sockets are closed on `Release()` instead of being retained.
- Idle entries expire after a fixed 60 seconds, but expiry is swept only by `Acquire`. `Size` and
  `Available` can therefore count expired sockets until the next acquire.
- Ownership is explicit: `Acquire` returns a handle the caller owns (the pool retains its own
  independent reference on every tracked entry), so a pooled connection stays valid even after
  every caller reference is dropped. `Release(conn)` returns the handle to the pool for reuse —
  the caller's reference remains valid to hold or drop, but the caller must not perform further
  I/O on a released handle. An otherwise healthy untracked TCP handle may be adopted; if the
  pool is full its transport is closed instead (the caller's reference is untouched).
- Adoption is exclusive across pools. Each Tcp carries an atomic pool lease token. Releasing a Tcp
  that is still tracked by a different pool traps without adopting or closing it, so two pools can
  never publish the same transport to different borrowers. The lease is held through close to
  prevent an ownership-check/close race.
- `Clear()` closes idle entries only. Checked-out entries are detached from the pool without
  being closed, so a handle currently held by an `Acquire` caller keeps working; a later
  `Release` of a detached handle re-adopts or closes it. The same protection applies when the
  pool itself is garbage-collected.
- The reuse health probe rejects sockets with pending unread bytes: a connection released with
  an unread response is closed rather than re-pooled, so stale protocol data is never handed to
  the next caller. Native readiness-probe errors are also treated as unhealthy rather than as an
  idle reusable connection.
- Every pool operation validates and temporarily retains the pool before locking it. A GC
  finalizer therefore cannot destroy the mutex while an operation is active; mutex
  initialization failure prevents construction instead of publishing an unsynchronized pool.
- It does not track TLS hostname verification, ALPN, or certificate policy; use `HttpClient` for HTTPS-aware pooling.

> **Typing note:** `Acquire` is registered as `obj<Zanna.Network.Tcp>`, so both language frontends
> preserve the returned transport type.

---

## Zanna.Network.Multipart

Multipart form-data builder and parser for HTTP file uploads.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.Multipart.New()` - Create a new multipart builder

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `AddField(name, value)` | Multipart | Add a text field (fluent) |
| `AddFile(name, filename, data)` | Multipart | Add a file (fluent) |
| `Build()` | Bytes | Build the complete multipart body (traps on overflow/allocation failure) |
| `Parse(contentType, body)` | Multipart | Parse a multipart body; strict and atomic (traps on malformed input) |
| `ParseResult(contentType, body)` | Result | Non-trapping parse: `Ok(Multipart)` or `ErrStr` |
| `HasField(name)` | Boolean | True when a non-file field with the name exists |
| `HasFile(name)` | Boolean | True when a file part with the name exists |
| `GetField(name)` | String | Get field value from parsed multipart |
| `GetFile(name)` | Bytes | Get file data from parsed multipart |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `ContentType` | String | Content-Type header with boundary |
| `Count` | Integer | Number of parts |

### Multipart Notes

- Builder output escapes quote and backslash characters in `name=` / `filename=` values. Every
  ASCII control character and DEL is replaced with a space, so names and filenames are sanitized
  rather than round-tripped exactly.
- Parser accepts quoted or bare-token multipart parameters, including quoted `boundary=` values and escaped quotes inside `Content-Disposition`.
- Builder boundaries contain 40 random alphanumeric characters. The parser accepts only a limited
  MIME boundary character set and at most 63 bytes; parsed names and filenames must fit in a
  255-byte buffer. Each part-header block is capped at 64 KiB and the parsed body is capped at
  64 MiB.
- Empty fields, zero-byte file parts, and embedded NUL bytes in field values are preserved. Part
  names, filenames, and Content-Type boundary text reject embedded NUL bytes because they are
  serialized into MIME headers.
- Parsing requires CRLF between part headers and content and accepts a preamble. Parsing is
  strict and atomic: invalid content types or boundaries, oversized bodies, missing delimiters,
  delimiter-prefix false matches, unterminated quoted parameters, malformed part headers
  (including unnamed parts), truncated bodies without a closing boundary, and allocation
  failures all trap instead of returning an empty or partial object.
  A returned `Multipart` therefore always represents the complete input. Use `ParseResult` when
  you want a `Result` instead of a trap.
- Duplicate names are returned in first-part order. Use `HasField`/`HasFile` to distinguish a
  missing name from a present empty field or zero-byte file — `GetField`/`GetFile` still return
  empty values for missing names. `Build()` traps on size overflow or allocation failure rather
  than returning empty Bytes; a valid builder with no parts produces the terminal boundary.
- Multipart receivers carry a stable runtime class identity. Passing another managed object to a
  mutating, serialization, or property API traps before private payload access; predicates and
  missing-value getters reject invalid receivers safely.
- `Build()` performs checked sizing and writes directly into one exact-size native staging buffer,
  without allocating escaped copies per part. Native staging and partial managed results are
  released before an allocation trap propagates. Parsing uses the same atomic cleanup rule for
  partial parts, and `ParseResult` owns exactly one retained Multipart or error String reference.

`Build`, `Parse`, and `GetFile` are registered as unqualified object results. Use explicit
`Bytes`/`Multipart` locals or concrete receiver calls instead of relying on chained inference.

---

## Zanna.Network.NetUtils

Static network utility functions for port checking, CIDR matching, and IP classification.

**Type:** Static class

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `IsPortOpen(host, port, timeoutMs)` | Boolean | Check if a remote port accepts connections |
| `GetFreePort()` | Integer | Get a free (available) port on localhost |
| `MatchCidr(ip, cidr)` | Boolean | Check if an IPv4 address matches an IPv4 CIDR range (e.g., `"10.0.0.0/8"`) |
| `IsPrivateIp(ip)` | Boolean | Check if an IPv4 address is RFC 1918 private or loopback |
| `LocalIpv4()` | String | Get route-selected local IPv4 address, or loopback on failure |

`IsPortOpen` uses 1,000ms when `timeoutMs <= 0` and applies the timeout as a single monotonic
deadline across every resolved address, so the total call honors `timeoutMs` regardless of how
many candidates DNS returns. It probes IPv4 and IPv6 sequentially. A timeout above `2147483647`
returns false. Hosts containing an embedded NUL byte are rejected (returning false) rather than
probed as a truncated prefix. A candidate is skipped if nonblocking mode cannot be enabled, and
a connect counts as successful only when the `SO_ERROR` status read itself succeeds and reports
no error — a true result means a verified successful connect.

`GetFreePort` binds an IPv4 `127.0.0.1:0` socket and closes it, so another process can claim the
returned port before the caller binds; it returns `0` on failure. For production listeners, bind
the server itself to port 0 and read back its assigned port. `MatchCidr` is IPv4-only, treats a
network without `/prefix` as `/32`, and requires strict numeric addresses. `IsPrivateIp` recognizes
only RFC 1918 space plus `127.0.0.0/8`; it is not a complete local/special-use-address or trust
decision. `LocalIpv4` reports the IPv4 source address selected for a UDP route to `8.8.8.8:53`, or
`127.0.0.1` on failure; it does not enumerate interfaces and is not necessarily a stable “primary”
address.

---

## Zanna.Network.WsServer

WebSocket server that accepts upgrade requests and manages connected clients.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.WsServer.New(port)` - Create server on the given port

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Start()` | void | Start accepting WebSocket connections in background |
| `Stop()` | void | Stop server and disconnect all clients |
| `SetSubprotocol(protocol)` | void | Require a specific subprotocol for future upgrades |
| `Broadcast(message)` | void | Send text message to all connected clients |
| `BroadcastBytes(data)` | void | Send binary data to all connected clients |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `ClientCount` | Integer | Number of currently connected clients |
| `Subprotocol` | String | Required subprotocol for future upgrades, if any |
| `Port` | Integer | Listening port number |
| `IsRunning` | Boolean | True if server is accepting connections |

`WsServer` accepts ports 0–65535 (0 requests an OS-assigned ephemeral port; `Port` reports the bound value after `Start()`) and keeps up to 128 accepted connections visible to lifecycle management. Upgrade requests use strict CRLF-terminated HTTP/1.1 syntax, at most 100 headers and 16 KiB total request bytes. They require one canonical RFC 6455 key, Host and version; duplicate security singletons, folded or bare-LF fields, transfer coding, nonzero request bodies, malformed authorities and ports, and non-canonical keys are rejected. A configured subprotocol must be offered, and a present browser `Origin` must match the request scheme, host, and effective port.

After a plain WebSocket upgrade, each client is serviced by a worker-pool frame loop with the
same protocol behavior as `WssServer`: PING is answered with PONG, a client CLOSE is echoed and
the slot released (so `ClientCount` reflects upgraded live peers), framing violations and invalid
UTF-8 text close the connection with the appropriate status code, and data frames are drained.
Text validation is incremental across fragments, including code points split between frames, and
both individual frames and aggregate messages are capped at 64 MiB. Broadcast writes and worker
control writes use a per-client I/O lock so frames never interleave on one socket.

Post-upgrade client sockets carry a two-second send bound and per-client write locks. The worker
pool is created by `Start()`, sized from logical CPU count (minimum 4, maximum 32), and shut down,
joined, and released by `Stop()`; restarting creates a fresh pool. Each serviced client occupies
one worker, while additional accepted sockets remain generation-tagged and visible to Stop until
a worker is available. `Stop()` actively closes queued/plain handshakes, joins all tasks, and does
not leave idle worker threads behind. Text broadcasts carry the full runtime byte length, so
embedded NUL bytes are sent as payload data. Every public server handle has stable managed
identity; forged receivers and forged String/Bytes arguments trap before private state is read.

---

## Zanna.Network.WssServer

TLS-backed WebSocket server built on the in-tree TLS 1.3 runtime with zero external TLS dependencies.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.WssServer.New(port, certFile, keyFile)` - Create a secure WebSocket server on the given port using PEM credentials

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Start()` | void | Start accepting secure WebSocket connections in background |
| `Stop()` | void | Stop server and disconnect all clients |
| `SetSubprotocol(protocol)` | void | Require a specific subprotocol for future secure upgrades |
| `Broadcast(message)` | void | Send a text message to all connected clients |
| `BroadcastBytes(data)` | void | Send a binary message to all connected clients |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `ClientCount` | Integer | Number of currently connected secure WebSocket clients |
| `Subprotocol` | String | Required subprotocol for future secure upgrades, if any |
| `Port` | Integer | Listening TCP port number |
| `IsRunning` | Boolean | True if the TLS listener is accepting connections |

### TLS Credential Requirements

- `certFile` and `keyFile` follow the same PEM requirements as `HttpsServer`.
- The built-in secure WebSocket path runs the RFC 6455 HTTP upgrade over TLS 1.3 with ALPN `http/1.1`.
- Because the TLS stack is implemented in-tree, `WssServer` does not require OpenSSL, LibreSSL, mbedTLS, or platform TLS frameworks at runtime.

### Runtime Notes

- `WssServer` accepts ports 0–65535 (0 requests an OS-assigned ephemeral port; `Port` reports the bound value after `Start()`) and tracks at most 128 upgraded clients.
- `WssServer` automatically completes the same strict, CRLF-only, 16-KiB RFC 6455 HTTP upgrade after the TLS handshake succeeds. Duplicate singleton fields, obsolete folding, bare LF, request bodies, malformed authorities, and non-canonical keys are rejected.
- `SetSubprotocol(protocol)` makes the server require that token in the client's `Sec-WebSocket-Protocol` list and echoes it in the upgrade response.
- Browser-facing upgrades require a `Host` header, and when an `Origin` header is present it must match the request scheme, host, and effective port.
- Control frames are handled automatically: server-side pong replies are sent for client pings, and close frames are echoed so the WebSocket close handshake completes cleanly.
- Client text/binary frames are drained and validated so broadcasts continue to work on long-lived secure connections even when clients send their own traffic. Fragmented text keeps incremental UTF-8 state across frames, and frames/messages are capped at 64 MiB.
- `Start()` fails cleanly on listener-bind or accept-thread startup errors instead of leaving the server in a partial running state.
- TLS/client work runs in a lazily created worker pool sized from logical CPU count (minimum 4,
  capped at 32). Each connected client occupies a worker in a blocking receive loop, so the number of
  simultaneously *serviced* clients is bounded by the pool size; additional accepted sockets
  queue until a worker frees and are not included in `ClientCount`. Queued sockets and detached
  TLS-handshake descriptors remain generation-tagged and visible to `Stop()`.
- Sends are serialized per client, not globally: each client slot has its own write lock, and
  post-upgrade sockets carry a two-second send bound, so a slow or non-reading peer has bounded
  impact. TLS receive remains blocking for quiet clients and is interrupted explicitly by Stop.
  Text
  broadcasts carry the full runtime byte length (embedded NUL bytes are payload data).
- `Stop()` bidirectionally shuts down pending and active native sockets, drains and joins all
  accepted work, and releases its worker pool. A later `Start()` creates a new pool; stopped
  WSS instances do not accumulate idle threads.
- The secure upgrade reader bounds the whole request to 16 KiB and 100 header lines. Certificate/key paths are validated: NULL
  handles and paths containing embedded NUL bytes trap at construction.
- Every WSS receiver has stable managed identity. Credential, subprotocol, String, and Bytes
  handles are validated before their storage is inspected, and constructor allocation failures
  leave the exact managed-object baseline.

---

## Zanna.Network.SseClient

Server-Sent Events (SSE) client for receiving event streams over HTTP.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.SseClient.Connect(url)` - Connect to an SSE endpoint

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Recv()` | String | Receive next event data (blocking) |
| `RecvFor(timeoutMs)` | String | Receive with timeout |
| `RecvForResult(timeoutMs)` | Result | Timed receive as `Ok(data)` / `ErrStr` (timeout vs stream end) |
| `Close()` | void | Close the connection |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `IsOpen` | Boolean | True if connection is active |
| `LastEventType` | String | Most recent `event:` field value |
| `LastEventId` | String | Most recent `id:` field value |

### Stream Behavior

- Supports HTTP and HTTPS with connection-delimited, exact `Content-Length`, or exact
  `Transfer-Encoding: chunked` bodies. A response cannot combine transfer coding and content
  length, and a content-length body must end at precisely the declared byte count.
- The response head requires CRLF, strict status syntax, token-valid field names and valid field
  bytes. It is bounded to 16 KiB and 100 fields. Obsolete folding, bare LF, and duplicate
  `Content-Type`, `Content-Length`, `Transfer-Encoding`, `Content-Encoding`, or `Location` fields
  are rejected. Up to eight informational responses are consumed before the final response;
  protocol switching (101) is never accepted.
- The final response must be HTTP 200 with exactly the `text/event-stream` media type (ASCII
  case-insensitive; semicolon-separated parameters are allowed). Only identity content encoding
  and the single `chunked` transfer coding are supported. Unsupported or additional codings are
  rejected before event parsing.
- Chunk sizes and extensions are parsed strictly. Individual chunks are capped at 16 MiB;
  trailers share the bounded header grammar and cannot introduce framing fields. An event-stream
  line is capped at 64 KiB and accumulated event data at 4 MiB.
- URLs without an explicit port use 80 for `http://` and 443 for `https://`. Connect follows at
  most five redirects. A 301 or 308 permanently replaces the URL used by later reconnects;
  302/303/307 affect only the current connection attempt.
- When a stream ends, the client waits for the most recent valid `retry:` value (3,000 ms by
  default) and reconnects. A safe, nonempty event ID is sent as `Last-Event-ID`; IDs containing
  NUL are ignored, and control-bearing IDs are never emitted as HTTP headers.

The EventSource field algorithm is byte-exact. The parser removes one optional UTF-8 BOM at the
start of the stream, accepts CRLF, lone CR, and lone LF payload delimiters, supports fields with
or without a colon, and removes at most one leading space from a field value. Every `data` field,
including an empty one, participates in LF joining. A blank line dispatches only after at least
one data field. `LastEventType` describes the most recently dispatched event; an event without an
`event` field resets it to the default empty type rather than exposing an earlier event's type.

`Recv` and `RecvFor` serialize on each client. `RecvFor(0)` blocks like `Recv`; a positive timeout
is one monotonic whole-event deadline covering every read, partial line, chunk delimiter,
reconnect attempt, and retry delay. A trickling peer therefore cannot renew the budget. Timeout
is lossless: partial HTTP body framing, line bytes, and accumulated event data remain in parser
state, and the next receive resumes exactly where it stopped. Timeout does not close or reconnect
the stream. An incomplete final line at actual end-of-stream is discarded as required by the
EventSource algorithm.

The legacy String methods return an empty String for timeout or terminal close, which is
ambiguous with a valid empty-data event. Protocol/limit/allocation failures trap after the receive
owner restores its socket policy and releases its mutex. Use `RecvForResult(timeoutMs)` to receive
`OkStr(data)` for every delivered event, including empty data, and `ErrStr` for timeout
(`SSE: timeout`), close (`SSE: stream closed`), or a caught receive trap.

`Close` is safe concurrently with receive. It marks the stream closed and performs native socket
shutdown to wake active I/O, but the receive owner retains and finally releases TCP/TLS storage.
Multiple receive callers wait and consume complete events one at a time. `IsOpen`,
`LastEventType`, and `LastEventId` take synchronized native snapshots; returned Strings are
independent caller-owned values. `IsOpen` is a local-state query, not a remote liveness probe.
Every client has stable managed identity, and `Connect` rejects forged or embedded-NUL URL
Strings before network access.

---

## Zanna.Network.HttpClient

Session-based HTTP client with cookie jar, auto-redirect, and persistent headers.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.HttpClient.New()` - Create a new HTTP client session

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `Get(url)` | HttpRes | HTTP GET request |
| `Post(url, body)` | HttpRes | HTTP POST with string body |
| `Put(url, body)` | HttpRes | HTTP PUT with string body |
| `Delete(url)` | HttpRes | HTTP DELETE request |
| `SetHeader(name, value)` | void | Set a default header for all requests (replaces case-insensitive matches) |
| `SetPoolSize(max)` | void | Resize the internal keep-alive pool |
| `SetTimeout(ms)` | void | Set each request's end-to-end deadline |
| `SetMaxRedirects(max)` | void | Set maximum redirect count |
| `SetCookie(domain, name, value)` | void | Set a validated host-only cookie for exactly that host |
| `DeleteCookie(domain, name)` | void | Remove a stored cookie by exact domain and case-sensitive name |
| `GetCookies(domain)` | Map | Get the active cookies that match a domain |

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `FollowRedirects` | Boolean | Whether to auto-follow redirects (default: true) |
| `KeepAlive` | Boolean | Whether requests reuse pooled keep-alive sockets |

### Cookie Behavior

- `HttpClient` stores cookies with domain, path, expiry, and secure attributes.
- Response cookies initially use host-only matching unless a validated `Domain` attribute is
  accepted. The runtime has a small built-in public-suffix deny list, not a complete public suffix
  database.
- Path-scoped cookies are only sent to matching request paths.
- `Secure` cookies are only sent over `https://` requests.
- `Secure` cookies received over plain `http://` responses are rejected instead of being stored.
- `Domain` attributes are normalized before storage. Syntactically invalid `Max-Age` values are
  ignored; a valid value that would overflow its absolute expiry saturates to `INT64_MAX`.
- Expired cookies are purged automatically before requests and lookups.
- All response-cookie fields are parsed and allocated into detached native staging before the jar
  lock is acquired. The completed set is then published in one allocation-free critical section;
  native allocation failure leaves the pre-response jar unchanged instead of accepting a prefix.

`SetCookie` applies the same validation as response cookies — token-valid names, cookie-octet
values (no `;`, quotes, or controls), and syntactically valid domains — and traps on invalid
input. Manual cookies are stored **host-only**: a cookie set for `example.com` is sent to
exactly `example.com`, never its subdomains, which also makes broad domains like `com`
harmless (they only ever match the literal host). Empty values are stored as valid cookies;
use `DeleteCookie` to remove one. Cookie names are case-sensitive throughout the jar, so `SID`
and `sid` coexist and replace independently.

### Transport Behavior

- `HttpClient` enables transport reuse by default and maintains an internal per-client connection pool.
- The default pool limit is 8 (clamped to 1–64 when changed), and idle entries expire after 30
  seconds.
- Plain `http://` requests reuse HTTP/1.1 keep-alive sockets when the response framing is safe.
- `https://` requests negotiate `h2` first; an HTTP/2 connection is reused across sequential requests as new streams when the peer selects `h2`, otherwise the client falls back to pooled HTTP/1.1 keep-alive reuse.
- Sensitive default headers such as `Authorization`, `Cookie`, and common API-key headers are stripped automatically when a redirect crosses origin boundaries.
- Default-header, cookie-jar, redirect-policy, timeout, and pool mutations are synchronized
  internally. Each request retains a complete defaults/pool snapshot, so one `HttpClient` instance
  can be shared across concurrent request paths without holding the session lock during transport.
- Set `KeepAlive = false` to force the previous close-after-each-request behavior.
- Resizing while keep-alive is disabled records only the future capacity and retains no idle pool.
  Concurrent enable/resize operations recheck the configured size before publishing a new pool.
- Default headers replace case-insensitively (HTTP field names are case-insensitive), so the
  last `SetHeader` call for a name wins regardless of spelling and no duplicates reach the wire.
- Header names must be HTTP tokens; embedded NUL and CR/LF injection are rejected. Replacement is
  transactional, so allocation failure cannot remove the previous casing/value.
- The default timeout is one 30-second monotonic deadline per request, shared by name resolution,
  connection attempts, TLS, request and response I/O, redirects, and response transformations.
  `SetTimeout(0)` disables the timeout entirely — the value is applied to every request, so a
  zero-timeout client issues genuinely unbounded requests. Use with care for long-lived streams.

Each verb copies its URL and optional String body by exact runtime length, then owns request setup,
transport, response-cookie capture, and every redirect hop in one recoverable transaction. A trap
releases the current request, intermediate response, and redirect temporaries before preserving the
original network error category. POST changes to GET for 301/302 and every 303; 307/308 preserve the
method and body. Cross-origin hops stop forwarding sensitive default headers. Embedded NUL bytes are
valid in POST/PUT bodies but never in URLs.

`GetCookies(domain)` returns an independent caller-owned `Map<String,String>`. Native cookie bytes
are copied while locked, but all managed Map/String construction happens after unlocking; partial
snapshots are released if any allocation traps. Every non-null public receiver is also checked
against the stable HttpClient object identity before private synchronization or cookie state is
accessed.

`Get`, `Post`, `Put`, `Delete`, and `GetCookies` are registered as unqualified object results.
Assign them to explicit `HttpRes`/`Map` locals or call concrete receiver functions rather than
depending on chained member inference.

---

## Zanna.Network.SmtpClient

Simple SMTP client for sending emails with optional AUTH LOGIN and negotiated STARTTLS.

**Type:** Instance class

**Constructors:**
- `Zanna.Network.SmtpClient.New(host, port)` - Create SMTP client

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `SetAuth(username, password)` | void | Set AUTH LOGIN credentials |
| `SetTls(enable)` | void | Enable/disable TLS |
| `SendResult(from, to, subject, body)` | Result | Send plain text email as `OkI64(1)` or `ErrStr(message)` |
| `SendHtmlResult(from, to, subject, html)` | Result | Send HTML email as `OkI64(1)` or `ErrStr(message)` |
| `Close()` | void | Force-close any active transport without sending QUIT |

`SendResult(...)` and `SendHtmlResult(...)` attach a stable snapshot of the SMTP failure to the
operation itself, so a failure is never lost between the call and a later status read.
Receiver, String, TCP, and TLS traps are caught only after the operation has closed its transport
and released its mutex, then returned as `ErrStr(message)`. Result allocation itself can still
trap if the runtime cannot allocate the error String or Result object; partial values are released
first.

### Runtime Notes

- Every send opens a fresh connection, applies 30-second transport-operation timeouts, sends one
  recipient, and closes afterward. It begins with `EHLO localhost`. Plaintext sessions without
  TLS or AUTH requirements may fall back to HELO only for replies 500, 502, or 504; malformed
  replies and every other code remain failures.
- Port 465 enables implicit TLS by default. On other ports, `SetTls(true)` requires an advertised
  STARTTLS capability and a successful upgrade before any AUTH command. Both implicit TLS and
  STARTTLS publish the TLS owner before the blocking handshake, allowing concurrent Close to
  interrupt it without freeing live TLS buffers.
- `AUTH LOGIN` is attempted only when both credentials are configured, the server advertises
  LOGIN, and the transport is encrypted. Username and password must be supplied together, are
  capped at 16 KiB each, and replace the previous pair transactionally. Stored and base64-encoded
  secret bytes are wiped before native release. `SetAuth(NULL, NULL)` disables authentication.
- SMTP replies require CRLF. Each content line is capped at 510 bytes; C0 controls other than HTAB
  and DEL are rejected. A multiline reply may contain at most 100 lines, must repeat one status
  code, and terminates only with `code SP`. The parser reads through a 4 KiB native buffer rather
  than allocating one managed Bytes object per byte.
- Recipient replies 250, 251, and 252 are accepted so forwarding and local-alias deliveries
  interoperate. MAIL FROM and RCPT TO paths remain single-recipient and reject empty, oversized,
  whitespace/control-bearing, or angle-bracket-bearing values.
- MIME headers and the body stream directly through one fixed 4 KiB native staging buffer,
  independent of message size. Every C0 or DEL byte in an untrusted header value becomes a space.
  Body CR, LF, and CRLF delimiters normalize to CRLF, a dot at the start of every normalized line
  is doubled, and the runtime guarantees CRLF before the final dot terminator. HTML bodies are
  framed identically but are not escaped.
- Host, credential, and message Strings have stable managed identity and are checked for embedded
  NUL before C-string use. Construction rejects empty or URL-like hosts, invalid ports, and
  malformed bracketed IPv6 literals. A bracketed IPv6 host is normalized before DNS/TLS use.

Sends, `SetAuth`, and `SetTls` serialize per client. Two concurrent send callers complete separate
fresh SMTP sessions without sharing a transport or response buffer. Each result carries an
independent caller-owned String snapshot. `Close` may run concurrently: it marks
cancellation and shuts down the descriptor to wake blocked I/O, while the active send remains the
only owner allowed to detach and free TCP/TLS state. A later send clears cancellation and
reconnects. Successful DATA acceptance remains success even if the advisory QUIT exchange fails;
the transport is still closed deterministically.

---

## Zanna.Network.AsyncSocket

Future-based wrappers around blocking socket operations.

**Type:** Static class

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `SetPoolSize(n)` | void | Configure the worker pool size (1–1024); call before the first async operation |
| `ConnectAsync(host, port)` | Future | Async TCP connect → resolves to Tcp |
| `ConnectForAsync(host, port, timeoutMs)` | Future | Async TCP connect with an explicit timeout |
| `SendAsync(tcp, data)` | Future | Async send → resolves to a boxed Integer byte count |
| `RecvAsync(tcp, maxBytes)` | Future | Async receive → resolves to Bytes |
| `HttpGetAsync(url)` | Future | Async HTTP GET → resolves to String |
| `HttpPostAsync(url, body)` | Future | Async HTTP POST → resolves to String |

All methods return a runtime `Zanna.Threads.Future`; the registry signatures use the qualified
`obj<Zanna.Threads.Future>` type. The payload type varies by operation as listed above, so inspect
the Future error state before retrieving and casting/unboxing its value.

### Failure Behavior

- Transport and HTTP failures resolve the returned `Future` as an error instead of trapping out of the worker thread.
- Argument-validation failures trap synchronously before submission and stop immediately —
  with a returning trap hook installed, each rejected input raises exactly one trap and the
  call returns a failed Future (or NULL) instead of continuing.
- Connect ports must be 1–65,535. Explicit connect timeouts and receive limits must be
  0–`INT_MAX` milliseconds/bytes. `SendAsync` requires live managed Tcp and Bytes handles; both
  are retained and revalidated before crossing the worker boundary.
- Async connect and HTTP GET/POST reject host or URL strings with embedded NUL bytes. `HttpPostAsync` preserves the runtime byte length of its body, including embedded NUL bytes.
- Inspect the `Threads.Future.IsError` and `Threads.Future.Error` properties before calling
  `Get()` on a failed future.
- Worker rejection remains terminal even if copying its diagnostic runs out of memory. In that
  fallback case `IsError` is true, `Error` is empty, and `Get()` traps with the generic Future
  error diagnostic rather than leaving the operation pending.
- **Executor model:** one lazily created shared worker pool runs the ordinary blocking TCP/HTTP
  functions. It defaults to twice the logical CPU count (minimum 8, capped at 1,024) and can be
  configured with `SetPoolSize(n)` before the first async operation (resizing after the pool
  exists traps). Each in-flight operation occupies one worker for its duration, so the pool size
  bounds concurrent operations; excess submissions queue. Configuration and first use are
  atomically serialized. If Pool construction fails, that operation receives an error Future and
  a later call may configure or retry initialization; failure is not cached permanently.
- **Cancellation model:** cancelling a returned Future changes only Future state — it does not
  interrupt the worker or its blocking socket operation, which runs to completion (its result or
  side effects are then discarded). Close the underlying `Tcp` to force a blocked operation to
  fail promptly.

Result publication is transfer-based: workers hand their freshly produced Tcp/Bytes/String/Box
reference directly to the Promise/Future pair and then release their producer Promise reference.
Future getters do **not** consume or clear that stored result; every successful getter returns a
separately retained caller reference that must be released at the C ABI layer. Finalizing the
Future releases the stored reference after worker cleanup. `SendAsync` resolves to a **boxed
Integer** (the same convention as `Async.Run` results): read it with `Zanna.Core.Box` accessors
such as `ToI64`.

---

## See Also

- [Collections](collections/README.md) - `Bytes`, `Map`, `Seq` types used by network classes
- [Input/Output](io/README.md) - File operations for saving downloaded content
- [Cryptography](crypto.md) - `Tls` for secure connections

> **Note:** `Zanna.Crypto.Tls` provides a low-level TLS 1.3 client API (connect/send/recv/close) that can be used independently of the HTTP layer. It supports AES-128-GCM-SHA256 and ChaCha20-Poly1305-SHA256 encryption with X25519 key exchange, IPv4/IPv6 connections, handshake timeouts, and HelloRetryRequest retry cookies for the X25519 path. When `verify_cert=1` (the default), it performs TLS 1.3 authentication using the runtime trust source plus the server-supplied chain, hostname verification against SubjectAltName/CommonName/IP SANs, leaf-certificate TLS-server-auth EKU/key-usage checks, and in-tree CertificateVerify signature verification. Documentation for this class is in `crypto.md`.

---

## Implementation Notes

### Threading

Raw `Tcp`, `Udp`, and `WebSocket` handles do not serialize concurrent reads or writes; use them
from one thread at a time or provide external coordination. Higher-level types such as
`ConnectionPool` and `HttpClient` document their own synchronization guarantees. `HttpRouter`,
`Multipart`, and `RateLimiter` document narrower mutation rules. `SseClient` serializes receive
callers and permits Close/metadata snapshots concurrently. `SmtpClient` serializes sends and
configuration changes, and allows Close to cancel the active operation without racing transport
destruction.
`TcpServer` supports concurrent accept waiters and concurrent close with the lifecycle guarantee
documented above. Other server types protect selected bookkeeping, but the WebSocket-server
caveats above still apply.

### Connection Limits

TCP listeners pass `SOMAXCONN` to the operating system. The effective accept backlog is controlled
and may be capped by the host platform and its network configuration.

### Resource Cleanup

Call `Close()` when a connection or server is no longer needed to release its socket
deterministically. Network objects also register finalizers that close an open socket when the
last runtime handle is reclaimed.

### Address Families

TCP, UDP, DNS, HTTP, TLS, WebSocket, SSE, SMTP, and `NetUtils.IsPortOpen` resolve with `AF_UNSPEC`
and can attempt IPv4 and IPv6. Wildcard listeners prefer a dual-stack IPv6 socket when the
operating system permits it and fall back to IPv4; an explicitly bound address uses that address's
family. `NetUtils.GetFreePort`, `MatchCidr`, `IsPrivateIp`, and `LocalIpv4` are intentionally
IPv4-only.
