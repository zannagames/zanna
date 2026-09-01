---
status: active
audience: public
last-verified: 2026-09-01
---

# Chapter 22: Networking

Every program you have written so far lives in isolation. It runs on your computer, works with local files, and interacts only with the person sitting at the keyboard. But the most transformative programs in history are not the ones that compute in solitude --- they are the ones that *connect*.

Think about the applications you use most. Email connects you to anyone in the world. Web browsers connect you to billions of pages of information. Multiplayer games connect you to friends and strangers across continents. Chat applications, video calls, social networks, online banking, streaming services, collaborative documents --- all of them share one essential capability: they communicate over a network.

Learning networking transforms you from someone who writes programs into someone who writes *systems*. A calculator computes. A chat application creates community. A weather app brings the world's meteorological data to your fingertips. A multiplayer game lets people who have never met share experiences. When your programs can talk to other computers, you can build things that no single computer could achieve alone.

This chapter teaches you to make your programs speak to the world. We will start with the concepts --- the mental models that make networking make sense --- and then build practical skills from simple web requests to full chat applications. By the end, you will understand how data flows across the internet and have the tools to build connected applications of your own.

---

## The Postal System Analogy

Before we look at any code, let's build a mental model. Networking can seem magical and opaque, but it follows rules that are remarkably similar to something you already understand: the postal system.

Imagine you want to send a letter to a friend in another city. You cannot simply think the message to them. You need infrastructure --- a system that physically carries your words from your location to theirs.

Here's what you do:

1. **Write the message** on paper
2. **Put it in an envelope** with your friend's address and your return address
3. **Drop it in a mailbox** (hand it to the postal system)
4. **The postal system routes it** through sorting facilities, trucks, and planes
5. **A mail carrier delivers it** to your friend's mailbox
6. **Your friend opens and reads it**

Network communication works almost identically:

1. **Your program creates data** (a message, a request, game state)
2. **It wraps the data in a packet** with a destination address and your address
3. **It hands the packet to the operating system** which manages the network hardware
4. **The packet travels through routers** and switches across the internet
5. **It arrives at the destination computer**
6. **A program on that computer receives and processes it**

This analogy extends further. Just as the postal system has different services (regular mail, express delivery, registered mail, packages), computer networks have different protocols for different needs. Just as you need to know your friend's address to send a letter, your program needs to know the other computer's address to send data.

### Addresses: Where Does the Letter Go?

In the postal system, an address has multiple parts: street address, city, state, country, postal code. This hierarchical structure lets the postal system route mail efficiently --- first to the right country, then the right city, then the right street.

Network addresses work similarly. Every computer on the internet has an **IP address** --- a numerical label like `192.168.1.100` or `172.217.14.206`. An IP address is like a street address for computers. When you want to send data to another computer, you need its IP address.

But here's a twist: a single computer might be running many programs that all want to receive network data. Your computer might be running a web browser, an email client, and a game simultaneously. How does incoming data find the right program?

This is where **ports** come in. A port is like an apartment number within a building. The IP address gets the data to the right computer (the building), and the port number gets it to the right program (the apartment).

```text
IP Address: 192.168.1.100     (which computer)
Port:       8080              (which program on that computer)

Complete address: 192.168.1.100:8080

Analogy:
Street Address: 123 Main Street    (which building)
Apartment:      #4B                (which unit in the building)
```

Some ports have well-known purposes, like permanent businesses at fixed locations:
- Port 80: Web traffic (HTTP)
- Port 443: Secure web traffic (HTTPS)
- Port 25: Email (SMTP)
- Port 22: Secure shell (SSH)

When you visit a website, your browser automatically uses port 443 (HTTPS) or port 80 (HTTP) because those are the standard ports for web servers. It's like knowing that the post office is always in the government district of town --- you don't need to look up the address every time.

---

## Two Types of Mail: TCP vs UDP

The postal system offers different delivery options with different guarantees. Regular mail is cheap but slow, and you don't get confirmation that it arrived. Registered mail costs more but you get delivery confirmation and tracking. Express delivery prioritizes speed over cost.

Computer networks have similar options. The two most important are **TCP** (Transmission Control Protocol) and **UDP** (User Datagram Protocol). Understanding when to use each is crucial for building effective networked applications.

### TCP: Registered Mail with Receipts

TCP is like registered mail with delivery confirmation. When you send data using TCP, you get strong guarantees:

**Guaranteed delivery**: Every packet you send will arrive. If a packet gets lost in transit (and they do --- the internet is imperfect), TCP automatically detects the loss and resends it. You don't have to worry about missing data.

**Ordered arrival**: Packets arrive in the order you sent them. If you send "Hello" then "World", the receiver gets "Hello" first and "World" second, even if "World" actually traveled faster through the network. TCP holds "World" until "Hello" arrives and delivers them in order.

**Error checking**: TCP verifies that data wasn't corrupted in transit. If a cosmic ray flips a bit or electrical noise garbles some bytes, TCP catches it and requests a resend.

**Connection-based**: TCP requires establishing a connection before sending data, like a phone call. You dial, the other side answers, then you can talk back and forth until one of you hangs up.

Here's what TCP communication looks like conceptually:

```text
Your Computer                                      Server
     |                                                |
     |-------- "Hello, I want to connect" ---------> |
     |<------- "OK, I acknowledge" ------------------ |
     |-------- "Great, connection established" ----> |
     |                                                |
     |  (Connection is now open, like a phone call)  |
     |                                                |
     |-------- "Please send me the homepage" ------> |
     |<------- "Here is the homepage (part 1)" ----- |
     |<------- "Here is the homepage (part 2)" ----- |
     |<------- "Here is the homepage (part 3)" ----- |
     |-------- "Got it all, thanks!" ---------------> |
     |                                                |
     |-------- "Goodbye" --------------------------> |
     |<------- "Goodbye" ---------------------------- |
     |                                                |
     |  (Connection is closed)                       |
```

The three-step process at the beginning is called the "three-way handshake." It ensures both sides are ready before data starts flowing.

**When to use TCP**: Any time you need reliable, ordered delivery. Web browsing, email, file transfers, remote login, database connections, chat applications, most APIs. If losing data or receiving it out of order would cause problems, use TCP.

### UDP: Postcards, No Tracking

UDP is like sending a postcard. It's simple, fast, and lightweight, but you get no guarantees:

**No guaranteed delivery**: Packets might get lost. UDP doesn't track what was sent or received. If a packet disappears, nobody automatically notices or resends it.

**No ordering**: Packets might arrive out of order. If you send "Hello" then "World", the receiver might get "World" first. Or might get neither. Or might get both in the right order. UDP doesn't care.

**No connection**: UDP is connectionless. You simply send data to an address and hope it arrives. No handshake, no confirmation, no ongoing session. Each packet is independent, like individual postcards.

**Lightweight and fast**: Because UDP skips all the reliability overhead, it's significantly faster. No waiting for acknowledgments, no retransmission delays, no ordering buffers.

```text
Your Computer                                      Server
     |                                                |
     |-------- "Here's my position: x=10, y=20" ---> |
     |-------- "Here's my position: x=11, y=20" ---> |
     |-------- "Here's my position: x=12, y=21" ---> |
     |                (no responses needed)          |
     |                                                |
```

**When to use UDP**: When speed matters more than perfect reliability, and when you can tolerate or handle lost data yourself. Video streaming (a dropped frame is better than a delayed one), online games (old player positions are worthless, only current positions matter), voice calls (late audio is useless), DNS lookups, live broadcasts.

### The Tradeoff in Action

Imagine you're building a multiplayer game. You need to send two types of data:

1. **Player positions** (60 times per second) --- If a position update gets lost, who cares? Another update comes in 16 milliseconds. If an old position arrives late, it's worthless garbage. Use **UDP**.

2. **Chat messages** --- Losing a chat message means missing part of the conversation. Messages out of order make no sense. Delivery matters more than instant arrival. Use **TCP**.

Many games use both protocols simultaneously: UDP for time-sensitive game state, TCP for chat and reliable game events (score updates, player joined/left).

### Summary: TCP vs UDP

| Characteristic | TCP | UDP |
|---------------|-----|-----|
| Delivery guarantee | Yes | No |
| Order guarantee | Yes | No |
| Error checking | Yes | Minimal |
| Connection required | Yes | No |
| Speed | Slower | Faster |
| Overhead | Higher | Lower |
| Use when... | Reliability matters | Speed matters |
| Examples | Web, email, file transfer | Games, streaming, voice |

---

## Client-Server Architecture

When two computers communicate, they typically play different roles. One computer *provides* a service; the other *uses* it. The provider is called the **server** (it *serves* something). The user is called the **client** (like a customer at a restaurant).

Think about a restaurant. The kitchen prepares food and waits for orders. Customers come in, place orders, receive food, and leave. The kitchen doesn't seek out hungry people --- it waits for them to arrive. Customers don't cook --- they request from the kitchen.

```text
CLIENT-SERVER MODEL

      +---------+                     +---------+
      |         |                     |         |
      | Client  |                     | Server  |
      |         |                     |         |
      | (asks)  |-------Request------>| (has    |
      |         |                     |  stuff) |
      |         |<------Response------|         |
      | (gets)  |                     | (gives) |
      |         |                     |         |
      +---------+                     +---------+

   Your web browser              Google's web server
   Your email app                Gmail's mail server
   Your game client              Game company's server
```

### The Server's Job

A server's job is to:
1. **Wait** for clients to connect (like a restaurant waiting for customers)
2. **Accept** incoming connections
3. **Process** requests from clients
4. **Send** responses back
5. **Handle multiple clients** (often simultaneously)

Servers typically run continuously. A web server runs 24/7, always ready to respond to browsers. A game server stays online so players can connect whenever they want.

### The Client's Job

A client's job is to:
1. **Know** the server's address (IP and port)
2. **Connect** to the server
3. **Send** requests (what do you want from the server?)
4. **Receive** responses (what did the server send back?)
5. **Disconnect** when finished

Clients are typically temporary. You open a browser, visit some pages, then close it. You launch a game, play for a while, then quit. The client exists only when you need it.

### A Visual Trace: What Happens When You Visit a Website

Let's trace through exactly what happens when you type `www.example.com` into your browser and press Enter:

```text
Step 1: DNS Lookup (What's the IP address?)
+---------+                            +-----------+
| Browser |----"Where is example.com?"->| DNS      |
|         |<---"It's at 93.184.216.34"--|  Server   |
+---------+                            +-----------+

Step 2: TCP Connection (Let's establish communication)
+---------+                            +-----------+
| Browser |-------- SYN ---------------->| Web      |
|         |<------- SYN-ACK ------------|  Server   |
|         |-------- ACK ----------------->|          |
+---------+                            +-----------+
   (Three-way handshake complete)

Step 3: HTTP Request (What do you want?)
+---------+                            +-----------+
| Browser |----"GET / HTTP/1.1"-------->| Web      |
|         |    "Host: example.com"      |  Server   |
+---------+                            +-----------+

Step 4: Server Processing
           +-----------+
           | Web       |
           |  Server   |  "They want the homepage..."
           |           |  "Let me find that file..."
           |           |  "Preparing the response..."
           +-----------+

Step 5: HTTP Response (Here's what you asked for)
+---------+                            +-----------+
| Browser |<---"HTTP/1.1 200 OK"-------| Web      |
|         |    "Content-Type: text/html"|  Server   |
|         |    "<html><body>..."        |          |
+---------+                            +-----------+

Step 6: Connection Close
+---------+                            +-----------+
| Browser |-------- FIN ---------------->| Web      |
|         |<------- ACK ----------------|  Server   |
|         |<------- FIN ----------------|          |
|         |-------- ACK ----------------->|          |
+---------+                            +-----------+
```

All of this happens in milliseconds. The complexity is hidden behind simple functions like `Http.Get()`.

---

## The Internet in 60 Seconds (Revisited)

Now that you understand addresses, protocols, and client-server architecture, let's revisit what happens when you access a website:

1. **DNS lookup**: Your computer asks "Where is example.com?" and gets back `93.184.216.34` (the IP address)
2. **TCP connection**: Your computer connects to that address on port 443 (HTTPS) or 80 (HTTP)
3. **Request**: Your browser sends "Give me the homepage"
4. **Processing**: The server finds and prepares the response
5. **Response**: The server sends back HTML, CSS, JavaScript, images
6. **Rendering**: Your browser assembles and displays everything
7. **Disconnect**: The connection closes (or stays open for more requests)

Programs can do all of this too. That's networking.

---

## Making HTTP Requests

The simplest networking is fetching web pages and APIs. HTTP (Hypertext Transfer Protocol) is the language of the web --- the format that browsers and servers use to communicate.

```zia
bind Zanna.Network;
bind Zanna.Terminal as Terminal;
bind Fmt = Zanna.Text.Fmt;

func start() {
    // Simple fetch — returns the body as a string
    var body = Http.Get("https://api.example.com/data");
    Terminal.Say(body);

    // For more control, use HttpReq/HttpRes:
    var req = HttpReq.New("GET", "https://api.example.com/data");
    req.SetTimeout(5000);
    var res = HttpReq.Send(req);
    if HttpRes.get_Status(res) == 200 {
        Terminal.Say("Got response:");
        Terminal.Say(HttpRes.BodyStr(res));
    } else {
        Terminal.Say("Error: " + Fmt.Int(HttpRes.get_Status(res)));
    }
}
```

Let's trace through what this code actually does:

1. `Http.Get()` is called with a URL
2. Internally, it parses the URL to extract the host (`api.example.com`) and path (`/data`)
3. It performs a DNS lookup to get the IP address
4. It opens a TCP connection to that IP on port 443 (HTTPS)
5. It sends an HTTP GET request
6. It waits for and reads the response
7. `Http.Get()` returns the response body as a string

Use `HttpReq` and `HttpRes` when you need the status code, headers, timeout configuration, or TLS options.

All of that complexity is hidden behind one function call. This is abstraction at work.

### Understanding HTTP Methods

HTTP defines several methods (also called verbs) for different purposes. Each method signals a different *intent*:

**GET**: Retrieve data. "Give me something." This should not modify anything on the server. Reading, not writing. Safe to repeat.

```zia
// GET - retrieve data
var users = Http.Get("https://api.example.com/users");
```

**POST**: Create new data. "Here's something new." This typically modifies the server, adding new resources.

```zia
// POST - send data to create something new
var newUser = Http.Post(
    "https://api.example.com/users",
    "{\"name\":\"Alice\",\"email\":\"alice@example.com\"}"
);
```

**PUT**: Update existing data. "Replace what you have with this." Used to update resources that already exist.

```zia
// PUT - update existing data
var updated = Http.Put(
    "https://api.example.com/users/123",
    "{\"name\":\"Alice Smith\"}"
);
```

**DELETE**: Remove data. "Get rid of this." Deletes resources from the server.

```zia
// DELETE - remove data
var deleted = Http.Delete("https://api.example.com/users/123");
```

These methods map to CRUD operations (Create, Read, Update, Delete) that are fundamental to most applications:

| Operation | HTTP Method | Example |
|-----------|-------------|---------|
| Create | POST | Add a new user |
| Read | GET | View user profile |
| Update | PUT | Change user email |
| Delete | DELETE | Remove user account |

### Working with JSON APIs

Most modern web APIs exchange data in JSON (JavaScript Object Notation) format. JSON is a text format for structured data that's easy for both humans and computers to read.

```zia
bind Zanna.Network;
bind Json = Zanna.Data.Json;
bind Zanna.Terminal as Terminal;
bind Fmt = Zanna.Text.Fmt;
bind Codec = Zanna.Text.Codec;

class Weather {
    expose temperature: Integer;
    expose conditions: String;
    expose humidity: Integer;

    expose func init(temperature: Integer, conditions: String, humidity: Integer) {
        self.temperature = temperature;
        self.conditions = conditions;
        self.humidity = humidity;
    }
}

func fetchWeather(city: String) -> Weather? {
    // Build the URL with the city parameter
    var url = "https://api.weather.example.com/current?city=" + Codec.UrlEncode(city);

    // Make the request with a timeout
    var req = HttpReq.New("GET", url);
    req.SetTimeout(5000);
    var response = HttpReq.Send(req);

    // Check if the request succeeded
    if !HttpRes.IsOk(response) {
        Terminal.Say("Request failed with status: " + Fmt.Int(HttpRes.get_Status(response)));
        return null;
    }

    // Parse the JSON response
    // The response body might look like:
    // {"temp": 72, "conditions": "Sunny", "humidity": 45}
    var data = Json.Parse(HttpRes.BodyStr(response));

    // Extract the fields we need
    return new Weather(
        Json.GetInt(data, "temp"),
        Json.GetStr(data, "conditions"),
        Json.GetInt(data, "humidity")
    );
}

func start() {
    var weather = fetchWeather("Seattle");

    if weather != null {
        Terminal.Say("Temperature: " + Fmt.Int(weather.temperature) + "F");
        Terminal.Say("Conditions: " + weather.conditions);
        Terminal.Say("Humidity: " + Fmt.Int(weather.humidity) + "%");
    } else {
        Terminal.Say("Could not fetch weather data");
    }
}
```

Let's trace through the JSON parsing:

```text
Server Response (text):
{"temp": 72.5, "conditions": "Sunny", "humidity": 45.0}

After Json.Parse():
data is now a JSON object where:
  data["temp"] is a JSON number containing 72.5
  data["conditions"] is a JSON string containing "Sunny"
  data["humidity"] is a JSON number containing 45.0

After extracting values:
weather.temperature = 72.5
weather.conditions = "Sunny"
weather.humidity = 45.0
```

---

## TCP: The Foundation

HTTP is built on top of TCP. When you need more control than HTTP provides, or when you're building your own protocol, you work directly with TCP sockets.

A **socket** is an endpoint for communication --- like a telephone in the phone call analogy. You create a socket, connect it to another socket, and then you can send and receive data.

### TCP Client

Here's how to connect to a server and communicate:

```zia
bind Zanna.Network;
bind Zanna.Terminal as Terminal;
bind Fmt = Zanna.Text.Fmt;

func start() {
    // Connect to a server
    // This is like dialing a phone number
    var socket = Tcp.ConnectFor("example.com", 80, 3000);

    // Check if connection succeeded
    if socket == null {
        Terminal.Say("Connection failed");
        return;
    }

    Terminal.Say("Connected!");

    // Send data (write to the socket)
    // This is like talking into the phone
    socket.SendStr("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");

    // Receive response (read from the socket)
    // This is like listening to the other person
    var response = socket.RecvStr(4096);
    Terminal.Say("Received " + Fmt.Int(response.Length()) + " bytes");
    Terminal.Say(response);

    // Close the connection (hang up the phone)
    socket.Close();
}
```

Let's trace through what happens:

```text
Step 1: Tcp.Connect("example.com", 80)
   - DNS lookup: "example.com" -> 93.184.216.34
   - Create a socket (OS allocates resources)
   - TCP three-way handshake with 93.184.216.34:80
   - Return the connected socket

Step 2: socket.SendStr("GET / HTTP/1.1\r\n...")
   - Convert string to bytes
   - Pass bytes to OS network stack
   - OS sends TCP packet(s) to server
   - Wait for acknowledgment

Step 3: socket.RecvStr(4096)
   - Wait for incoming data
   - Receive TCP packets from server
   - Reassemble into continuous byte stream
   - Return as string when connection closes or all data received

Step 4: socket.Close()
   - Send TCP FIN packet
   - Wait for server's FIN
   - Release OS resources
   - Socket can no longer be used
```

### TCP Server

A server listens for incoming connections instead of initiating them:

```zia
bind Zanna.Network;
bind Zanna.Terminal as Terminal;

func start() {
    // Create a server socket that listens on port 8080
    // This is like setting up a phone line that can receive calls
    var server = TcpServer.Listen(8080);
    Terminal.Say("Server listening on port 8080");

    // Server loop: accept and handle connections forever
    while true {
        // Wait for a client to connect
        // This is like waiting for the phone to ring, then answering
        Terminal.Say("Waiting for client...");
        var client = server.Accept();

        Terminal.Say("Client connected from " + client.Host);

        // Read what the client sent
        var message = client.RecvLine();
        Terminal.Say("Client said: " + message);

        // Send a response
        client.SendStr("Hello, client! You said: " + message + "\n");

        // Close this client connection
        // The server keeps running, ready for more clients
        client.Close();
        Terminal.Say("Client disconnected");
    }
}
```

The server's lifecycle:

```text
                +-------------------+
                |   TcpServer.      |
                |   Listen(8080)    |
                +--------+----------+
                         |
                         v
                +--------+----------+
                |                   |
                |   server.Accept() |<--+
                |   (waits...)      |   |
                +--------+----------+   |
                         |              |
                         | client       |
                         | connects     |
                         v              |
                +--------+----------+   |
                |  Handle client:   |   |
                |  - Read message   |   |
                |  - Send response  |   |
                |  - Close client   |   |
                +--------+----------+   |
                         |              |
                         +--------------+
                    (loop back to accept)
```

---

## Building a Chat Application

Let's apply what we've learned to build something real: a chat system where multiple clients can send messages that everyone sees. This demonstrates many networking concepts working together.

### Understanding the Architecture

```text
         +----------------+
         |  Chat Server   |
         |  (port 9000)   |
         +-------+--------+
                 |
     +-----------+-----------+
     |           |           |
+----+----+ +----+----+ +----+----+
| Client1 | | Client2 | | Client3 |
| (Alice) | |  (Bob)  | | (Carol) |
+---------+ +---------+ +---------+

When Alice sends "Hello":
1. Alice's client sends "Hello" to server
2. Server receives "Hello" from Alice
3. Server broadcasts "Alice: Hello" to ALL clients
4. Bob and Carol (and Alice) see "Alice: Hello"
```

### Chat Server

```zia
module ChatServer;

bind Zanna.Network;
bind Zanna.Collections;
bind Zanna.Terminal as Terminal;
bind Zanna.Time.Clock as Clock;

class ServerApp {
    // The server socket that accepts new connections
    hide server: TcpServer;

    // List of all connected clients
    hide clients: List[Tcp];

    // Flag to control the server loop
    hide running: Boolean;

    expose func init(port: Integer) {
        // Start listening on the specified port
        self.server = TcpServer.Listen(port);
        self.clients = [];
        self.running = true;
        Terminal.Say("Chat server started on port " + port);
    }

    // Main server loop
    expose func run() {
        while self.running {
            // Step 1: Check for new clients trying to connect
            // AcceptFor with a short timeout returns null if no one is waiting
            // (blocking Accept would freeze the server until someone connects)
            var newClient = self.server.AcceptFor(100);

            if newClient != null {
                // A new client connected!
                self.clients.Push(newClient);
                self.broadcast("*** A new user has joined ***");
                Terminal.Say("New client connected. Total clients: " + self.clients.Length);
            }

            // Step 2: Check each client for incoming messages
            var i = 0;
            while i < self.clients.Length {
                var client = self.clients[i];

                // Available checks if the client sent anything without blocking
                if client.Available > 0 {
                    var message = client.RecvLine();

                    if message == "" || message == "/quit" {
                        // Client disconnected or wants to leave
                        self.clients.RemoveAt(i);
                        self.broadcast("*** A user has left ***");
                        client.Close();
                        Terminal.Say("Client disconnected. Total clients: " + self.clients.Length);
                        // Don't increment i; the next client shifted into this position
                        continue;
                    }

                    // Broadcast the message to everyone
                    self.broadcast(message);
                    Terminal.Say("Broadcast: " + message);
                }

                i += 1;
            }

            // Small sleep to avoid consuming 100% CPU
            Clock.Sleep(10);
        }
    }

    // Send a message to all connected clients
    func broadcast(message: String) {
        for client in self.clients {
            client.SendStr(message + "\n");
        }
    }

    // Gracefully shut down the server
    func stop() {
        self.running = false;

        // Close all client connections
        for client in self.clients {
            client.SendStr("*** Server shutting down ***\n");
            client.Close();
        }

        // Close the server socket
        self.server.Close();
        Terminal.Say("Server stopped");
    }
}

func start() {
    var server = new ServerApp(9000);
    server.run();
}
```

Let's trace through a typical server session:

```text
Time 0:00 - Server starts
  server.listen(9000) - now accepting connections
  clients = []

Time 0:05 - Alice connects
  acceptNonBlocking() returns Alice's socket
  clients = [Alice]
  broadcast("*** A new user has joined ***") -> Alice sees this

Time 0:10 - Bob connects
  acceptNonBlocking() returns Bob's socket
  clients = [Alice, Bob]
  broadcast("*** A new user has joined ***") -> Alice and Bob see this

Time 0:15 - Alice sends "Hi everyone!"
  Alice's socket.hasData() returns true
  message = "Hi everyone!"
  broadcast("Hi everyone!") -> Alice and Bob see this

Time 0:20 - Bob sends "Hello Alice!"
  Bob's socket.hasData() returns true
  message = "Hello Alice!"
  broadcast("Hello Alice!") -> Alice and Bob see this

Time 0:30 - Alice sends "/quit"
  Alice's socket.hasData() returns true
  message = "/quit"
  clients = [Bob] (Alice removed)
  broadcast("*** A user has left ***") -> Bob sees this
  Alice's socket closed
```

### Chat Client

```zia
module ChatClient;

bind Zanna.Network;
bind Zanna.Terminal as Terminal;

class ClientApp {
    hide socket: Tcp;
    hide username: String;
    hide running: Boolean;

    expose func init(host: String, port: Integer, username: String) {
        self.username = username;
        self.running = true;

        // Connect to the server
        self.socket = Tcp.Connect(host, port);

        if self.socket == null {
            Terminal.Say("Could not connect to server at " + host + ":" + port);
            self.running = false;
            return;
        }

        Terminal.Say("Connected to chat server!");
        Terminal.Say("Type messages and press Enter. Type /quit to exit.");
        Terminal.Say("");
    }

    expose func run() {
        if !self.running {
            return;
        }

        while self.running {
            // Wait for user to type something
            var input = Terminal.TryAsk("").UnwrapOrStr("");

            if input == "/quit" {
                self.running = false;
                self.socket.SendStr("/quit\n");
                break;
            }

            // Send the message (prefixed with username)
            self.socket.SendStr(self.username + ": " + input + "\n");

            // Drain any waiting server messages after each send.
            self.receiveAvailable();
        }

        self.socket.Close();
        Terminal.Say("Disconnected from server.");
    }

    func receiveAvailable() {
        while self.socket.Available > 0 {
            var message = self.socket.RecvLine();
            if message == "" {
                Terminal.Say("*** Disconnected from server ***");
                self.running = false;
                return;
            }

            Terminal.Say(message);
        }
    }
}

func start() {
    var username = Terminal.TryAsk("Enter your username: ").UnwrapOrStr("guest");
    var client = new ClientApp("localhost", 9000, username);
    client.run();
}
```

This simple client stays single-threaded: it sends a line of input, then drains any messages that are already waiting on the socket. That is enough for a small demo and keeps the example portable across the current Zia front end.

For a production chat client, use a background receive loop so incoming messages can appear while the user is still typing.

```text
Single Thread
    |
    |   Terminal.TryAsk()
    |   socket.SendStr()
    |   while socket.Available > 0:
    |       socket.RecvLine()
    |       Terminal.Say()
    |
```

---

## UDP: Fast but Unreliable

UDP trades reliability for speed. When you can tolerate lost packets or have your own way of handling them, UDP's lower overhead and faster delivery are attractive.

### Basic UDP Communication

```zia
bind Zanna.Network;
bind Zanna.Terminal as Terminal;
bind Fmt = Zanna.Text.Fmt;

// UDP sender
func sendUdpMessage() {
    // Create a UDP socket
    var socket = Udp.New();

    // Send data - no connection needed!
    // Just specify destination address and port
    socket.SendToStr("192.168.1.100", 5000, "Hello!");

    // Can send to different destinations with same socket
    socket.SendToStr("192.168.1.101", 5000, "Hi there!");

    socket.Close();
}

// UDP receiver
func receiveUdpMessages() {
    // Bind to port 5000 - we'll receive anything sent here
    var socket = Udp.Bind(5000);
    Terminal.Say("Listening for UDP messages on port 5000...");

    while true {
        // Wait for and receive a packet
        var data = socket.RecvFrom(1024);

        // The socket remembers the sender of the most recent packet
        Terminal.Say("From " + socket.SenderHost() + ":" + Fmt.Int(socket.SenderPort()));
        Terminal.Say("Received " + Fmt.Int(data.Length) + " bytes");
    }
}
```

Notice the differences from TCP:
- No `connect()` or `accept()` --- just send to an address
- No persistent connection --- each packet is independent
- Receiver doesn't know who will send, just waits for any packet

### Game Networking with UDP

Games often need to send player state many times per second. Lost packets don't matter because new state arrives constantly. Here's a typical pattern:

```zia
bind Zanna.Network;
bind Zanna.Time as Time;
bind Convert = Zanna.Core.Convert;

// Player state that we'll send frequently
struct PlayerState {
    id: Integer;
    x: Number;
    y: Number;
    rotation: Number;
    health: Integer;
    timestamp: Integer;  // When this state was captured
}

class GameNetwork {
    hide socket: Udp;
    hide serverAddress: String;
    hide serverPort: Integer;

    expose func init(serverAddress: String, serverPort: Integer) {
        self.socket = Udp.New();
        self.serverAddress = serverAddress;
        self.serverPort = serverPort;
    }

    // Send our current state to the server
    // Called 60 times per second
    func sendState(state: PlayerState) {
        var data = packPlayerState(state);
        self.socket.SendToStr(self.serverAddress, self.serverPort, data);
        // Note: we don't wait for acknowledgment!
        // If this packet is lost, we'll send another in 16ms
    }

    // Receive states of other players from the server
    func receiveStates() -> List[PlayerState] {
        var states: List[PlayerState] = [];

        // Process all available packets (non-blocking)
        while self.socket.Port >= 0 {
            var data = self.socket.RecvFor(1024, 1);
            if data == null {
                break;
            }
            var state = unpackPlayerState(data);

            // Only use recent states - discard old ones
            var now = Time.Clock.NowMs();
            if now - state.timestamp < 1000 {  // Less than 1 second old
                states.Push(state);
            }
        }

        return states;
    }
}

// Convert state to transmittable format
func packPlayerState(state: PlayerState) -> String {
    // Simple text format (real games use compact binary)
    return state.id + "," + state.x + "," + state.y + "," +
           state.rotation + "," + state.health + "," + state.timestamp;
}

// Convert received data back to state
func unpackPlayerState(data: String) -> PlayerState {
    var parts = data.Split(",");
    return PlayerState {
        id: Convert.ToInt64(parts.Get(0)),
        x: Convert.ToDouble(parts.Get(1)),
        y: Convert.ToDouble(parts.Get(2)),
        rotation: Convert.ToDouble(parts.Get(3)),
        health: Convert.ToInt64(parts.Get(4)),
        timestamp: Convert.ToInt64(parts.Get(5))
    };
}
```

The timestamp field is crucial. If packet A (timestamp 100) arrives after packet B (timestamp 150), we should ignore A --- it's outdated information. TCP would give us packets in order, but with UDP, older packets might arrive late.

---

## WebSockets: Real-Time Web

HTTP was designed for request-response: the client asks, the server answers, done. But what if the server needs to send data to the client without being asked? What if you want continuous, two-way communication?

**WebSockets** provide full-duplex communication over a single connection. After an initial HTTP handshake, the connection stays open and either side can send messages at any time.

```zia
bind Zanna.Network;
bind Zanna.Terminal as Terminal;

class WebSocketClient {
    hide ws: WebSocket;
    expose connected: Boolean;

    expose func init() {
        self.connected = false;
    }

    expose func connect(url: String) {
        self.ws = WebSocket.ConnectFor(url, 5000);
        self.connected = self.ws != null && self.ws.IsOpen;

        if self.connected {
            Terminal.Say("Connected to server!");
            self.ws.Send("Hello server, I'm online!");
        }
    }

    expose func poll() {
        if !self.connected {
            return;
        }

        // Poll with a short timeout so the caller can keep updating the app.
        var message = self.ws.RecvFor(10);
        if message != "" {
            Terminal.Say("Server: " + message);
        }

        self.connected = self.ws.IsOpen;
    }

    expose func send(message: String) {
        if self.connected {
            self.ws.Send(message);
        } else {
            Terminal.Say("Not connected!");
        }
    }

    expose func close() {
        self.ws.Close();
    }
}

func start() {
    var client = new WebSocketClient();
    client.connect("wss://chat.example.com/room/general");

    // Now we can send messages and poll for incoming messages.
    while true {
        client.poll();
        var input = Terminal.TryAsk("").UnwrapOrStr("");
        if input == "/quit" {
            client.close();
            break;
        }
        client.send(input);
    }
}
```

WebSockets are ideal for:
- Real-time dashboards that update continuously
- Live chat applications
- Collaborative editing (Google Docs style)
- Live sports scores, stock tickers
- Multiplayer web games

---

## Handling Network Errors

Networks are inherently unreliable. Connections drop. Servers go down. Packets get lost. WiFi cuts out. Your programs must handle these failures gracefully.

### The Many Ways Networks Fail

```zia
bind Zanna.Network;
bind Zanna.Terminal as Terminal;
bind Fmt = Zanna.Text.Fmt;

func demonstrateFailures() {
    // Failure 1: Cannot connect
    // Server might be down, address might be wrong
    var socket = Tcp.ConnectFor("nonexistent.example.com", 80, 3000);
    if socket == null {
        Terminal.Say("Could not connect - server unreachable");
        return;
    }

    // Failure 2: Connection drops mid-conversation
    // WiFi cuts out, server crashes, network cable unplugged
    var response = socket.RecvLine();
    if response == "" {
        Terminal.Say("Connection lost while reading");
    }

    // Failure 3: Timeout - server too slow
    // Server overloaded, network congested
    // Without timeout, we might wait forever

    // Failure 4: Server returns error
    // We connected and communicated, but server said "no"
    var req = HttpReq.New("GET", "https://api.example.com/resource");
    var httpResponse = HttpReq.Send(req);
    var status = HttpRes.get_Status(httpResponse);
    if status == 404 {
        Terminal.Say("Resource not found");
    } else if status == 500 {
        Terminal.Say("Server error");
    }
}
```

### Robust Networking with Retries

For important operations, implement retry logic with exponential backoff:

> **Note:** the `return null;` statements below currently fail IL verification —
> returning `null` directly from a `String?` function is a known lowering defect
> ([audit #25](../../audit_09012026.md)). Return through a typed local
> (`var none: String? = null; return none;`) until it is fixed.

```zia
bind Zanna.Network;
bind Zanna.Terminal as Terminal;
bind Zanna.Time as Time;
bind Fmt = Zanna.Text.Fmt;

func robustFetch(url: String, maxRetries: Integer) -> String? {
    var retries = 0;

    while retries < maxRetries {
        // Set a timeout so we don't wait forever.
        var req = HttpReq.New("GET", url);
        req.SetTimeout(5000);
        var response = HttpReq.Send(req);
        var status = HttpRes.get_Status(response);

        if HttpRes.IsOk(response) {
            return HttpRes.BodyStr(response);  // Success!
        }

        if status >= 500 {
            // Server error (5xx) - worth retrying
            // The server might recover
            retries += 1;
            Terminal.Say("Server error " + Fmt.Int(status) +
                         ", retrying... (" + Fmt.Int(retries) + "/" + Fmt.Int(maxRetries) + ")");

            // Exponential backoff: wait longer each retry
            // 1st retry: 1 second, 2nd: 2 seconds, 3rd: 4 seconds
            var waitTime = 1000 * (1 << retries);
            Time.Clock.Sleep(waitTime);
            continue;
        }

        // Client error (4xx) - don't retry
        // Our request is wrong, retrying won't help
        Terminal.Say("Client error " + Fmt.Int(status) + " - not retrying");
        return null;
    }

    Terminal.Say("Failed after " + Fmt.Int(maxRetries) + " retries");
    return null;
}

func start() {
    var data = robustFetch("https://api.example.com/important-data", 5);

    if data != null {
        Terminal.Say("Got data: " + data);
    } else {
        Terminal.Say("Could not fetch data");
    }
}
```

### Why Exponential Backoff?

If a server is overloaded, hammering it with rapid retry attempts makes things worse. Exponential backoff (waiting longer after each failure) gives the server time to recover:

```text
Retry 1: Wait 1 second
Retry 2: Wait 2 seconds
Retry 3: Wait 4 seconds
Retry 4: Wait 8 seconds
Retry 5: Wait 16 seconds
```

This pattern is used throughout the internet. It's polite and effective.

### Timeouts: Don't Wait Forever

Always set timeouts:

```text
// Without timeout - could hang forever if server doesn't respond
var response = Http.Get(url);  // Dangerous!

// With timeout - give up after 5 seconds
var req = HttpReq.New("GET", url);
req.SetTimeout(5000);
var response = HttpReq.Send(req);

// For sockets, set timeouts explicitly
var socket = Tcp.ConnectFor(host, port, 3000);
socket.SetRecvTimeout(10000);   // 10 seconds to read
socket.SetSendTimeout(5000);    // 5 seconds to write
```

How long should a timeout be? Long enough for normal operations, short enough to catch real problems. Common values:
- Connect timeout: 3-10 seconds
- Read timeout: 10-30 seconds
- Total request timeout: 30-60 seconds

---

## Common Mistakes in Network Programming

Network programming has unique pitfalls. Here are the mistakes beginners make most often, and how to avoid them.

### Mistake 1: Forgetting to Close Connections

Every open connection uses system resources. Leaking connections will eventually crash your program or the system.

```text
// BAD: Connection leak!
func fetchData(host: String, port: Integer) -> String {
    var socket = Tcp.Connect(host, port);
    var data = socket.RecvStr(4096);
    return data;  // Socket never closed!
}
// If called 1000 times, you have 1000 open connections

// GOOD: Always close
func fetchData(host: String, port: Integer) -> String {
    var socket = Tcp.Connect(host, port);
    var data = socket.RecvStr(4096);
    socket.Close();  // Clean up!
    return data;
}

// EVEN BETTER: Handle errors too
func fetchData(host: String, port: Integer) -> String? {
    var socket = Tcp.Connect(host, port);

    if socket == null {
        return null;
    }

    try {
        var data = socket.RecvStr(4096);
        return data;
    } finally {
        // 'finally' runs whether try succeeded or failed
        socket.Close();
    }
}
```

### Mistake 2: Blocking the Main Thread

Network operations can take seconds. If your main thread waits for network responses, your program freezes.

```text
// BAD: Freezes entire program during fetch
func onButtonClick() {
    var data = Http.Get(slowUrl);  // User can't click anything for 10 seconds!
    updateDisplay(data);
}

// GOOD: Use a separate thread
bind Thread = Zanna.Threads.Thread;

func fetchWorker(arg: Any) {
    var data = Http.Get(slowUrl);
    queueMainThreadUpdate(data);
}

func onButtonClick() {
    Thread.Start(&fetchWorker, 0);
}
// Button click returns immediately, fetch happens in background
```

This is especially important in GUI applications and games. A frozen UI is a terrible user experience.

### Mistake 3: Ignoring Partial Reads

When you read from a socket, you might not get all the data at once. Networks deliver data in chunks.

```text
// BAD: Assumes all data comes at once
var message = socket.RecvStr(1024);  // Might get less than 1024 bytes!

// GOOD: Read until you have what you need
func readExactly(socket: Tcp, count: Integer) -> String {
    var result = "";

    while result.Length() < count {
        var chunk = socket.RecvStr(count - result.Length());
        if chunk == "" {
            // Connection closed before we got all data
            return null;
        }
        result += chunk;
    }

    return result;
}
```

### Mistake 4: Not Validating Input from the Network

Data from the network is untrusted. It might be malformed, malicious, or just wrong.

```text
// BAD: Trusts network data blindly
var packet = socket.RecvStr(1024);
var index = Convert.ToInt64(packet);
myArray[index] = value;  // What if index is negative? Or huge?

// GOOD: Validate everything
var packet = socket.RecvStr(1024);

if packet.Length() > 100 {
    Terminal.Say("Packet too large, ignoring");
    return;
}

var index = Convert.ToInt64(packet);

if index < 0 || index >= myArray.Length {
    Terminal.Say("Invalid index received, ignoring");
    return;
}

myArray[index] = value;
```

### Mistake 5: Sending/Receiving Without a Protocol

Both sides need to agree on message format. Without a clear protocol, you get gibberish.

```text
// BAD: No clear message format
socket.SendStr("Alice");
socket.SendStr("25");
socket.SendStr("Hello");
// Receiver gets "Alice25Hello" - how to separate?

// GOOD: Define a clear protocol
// Option 1: Newline-delimited
socket.SendStr("Alice\n");
socket.SendStr("25\n");
socket.SendStr("Hello\n");

// Option 2: Length-prefixed
func sendMessage(socket: Tcp, message: String) {
    var length = message.Length();
    socket.SendStr(Fmt.Int(length) + ":" + message);  // "5:Hello"
}

// Option 3: Use a standard format like JSON
socket.SendStr("{\"name\":\"Alice\",\"age\":25,\"message\":\"Hello\"}\n");
```

### Mistake 6: Not Handling Connection Resets

Servers restart. Connections break. Your code needs to detect this and recover.

```text
// BAD: Assumes connection lasts forever
while true {
    var message = socket.RecvLine();
    process(message);
}
// If socket breaks, RecvLine returns an empty string, and process("") may be wrong

// GOOD: Detect disconnection and reconnect
bind Zanna.Terminal as Terminal;
bind Zanna.Time as Time;

func reliableConnection(host: String, port: Integer) {
    var socket: Tcp? = null;

    while true {
        // Connect if needed
        if socket == null {
            Terminal.Say("Connecting...");
            socket = Tcp.ConnectFor(host, port, 5000);

            if socket == null {
                Terminal.Say("Connection failed, retrying in 5 seconds");
                Time.Clock.Sleep(5000);
                continue;
            }

            Terminal.Say("Connected!");
        }

        // Try to read
        var message = socket.RecvLine();

        if message == "" {
            // Connection broke
            Terminal.Say("Disconnected!");
            socket.Close();
            socket = null;
            continue;  // Loop will reconnect
        }

        process(message);
    }
}
```

---

## Security Considerations

Network programming introduces security risks that don't exist in standalone programs. Data travels through untrusted networks where it can be observed or modified.

### Use HTTPS, Not HTTP

HTTP sends data in plain text. Anyone on the network path can read it. HTTPS encrypts the connection.

```zia
// BAD: Password sent in plain text!
Http.Post("http://example.com/login",
          "{\"username\":\"alice\",\"password\":\"secret123\"}");
// Anyone on the network can see "secret123"

// GOOD: Encrypted connection
Http.Post("https://example.com/login",
          "{\"username\":\"alice\",\"password\":\"secret123\"}");
// Data is encrypted, observers see gibberish
```

Always use `https://` for anything sensitive: logins, personal data, financial information.

### Validate Server Certificates

HTTPS uses certificates to prove the server is who it claims to be. Don't disable certificate validation!

```zia
// LOCAL TEST ONLY: disables security for self-signed fixtures
var req = HttpReq.New("GET", url);
req.AllowInsecureCertificatesForTesting();
HttpReq.Send(req);
// You might be talking to an attacker pretending to be the server

// SAFE: Always verify (this is the default)
Http.Get(url);  // Certificate verified automatically
```

### Don't Trust Network Input

Anything received from the network should be treated as potentially malicious.

```text
// User sends a filename they want to download
var filename = socket.RecvLine();

// BAD: Might download any file!
var contents = File.ReadAllText("/data/" + filename);
// If filename is "../../../etc/passwd", you're exposing system files

// GOOD: Validate and sanitize
var filename = socket.RecvLine();

// Check for path traversal attacks
if filename.Contains("..") || filename.Contains("/") || filename.Contains("\\") {
    socket.SendStr("Invalid filename\n");
    return;
}

// Only allow certain file extensions
if !filename.EndsWith(".txt") && !filename.EndsWith(".json") {
    socket.SendStr("Invalid file type\n");
    return;
}

var contents = File.ReadAllText("/data/" + filename);
```

### Rate Limiting

Without limits, attackers can flood your server with requests.

```zia
bind Clock = Zanna.Time.Clock;

class RateLimitedServer {
    // Track requests per IP address
    hide requestCounts: Map[String, Integer];
    hide lastReset: Integer;
    hide maxRequestsPerMinute: Integer;

    expose func init() {
        self.requestCounts = new Map[String, Integer]();
        self.lastReset = Clock.NowMs();
        self.maxRequestsPerMinute = 100;
    }

    func handleRequest(client: Zanna.Network.Tcp) {
        var ip = client.Host;
        var now = Clock.NowMs();

        // Reset counts every minute
        if now - self.lastReset > 60000 {
            self.requestCounts = new Map[String, Integer]();
            self.lastReset = now;
        }

        // Check rate limit
        var count = self.requestCounts.Get(ip) ?? 0;

        if count >= self.maxRequestsPerMinute {
            client.SendStr("Rate limit exceeded. Please slow down.\n");
            client.Close();
            return;
        }

        // Record this request
        self.requestCounts.Set(ip, count + 1);

        // Process normally
        processRequest(client);
    }
}
```

### Keep Secrets Out of Code

Never embed passwords, API keys, or tokens in your source code.

```zia
// BAD: API key in source code
var hardCodedResponse = Http.Get("https://api.example.com/data?key=sk_live_abc123xyz");
// If someone sees your code, they have your key

// GOOD: Load from environment or config
var apiKey = Zanna.System.Environment.GetVariable("API_KEY");
var configuredResponse = Http.Get("https://api.example.com/data?key=" + apiKey);
```

---

## Debugging Network Issues

Network bugs are notoriously hard to track down. The problem might be in your code, in the network, or in the server you're talking to. Here's how to investigate.

### Print Everything

When network code doesn't work, add logging at every step:

```zia
bind Zanna.Network;
bind Zanna.Terminal as Terminal;
bind Fmt = Zanna.Text.Fmt;

func debugFetch(url: String) -> String {
    Terminal.Say("[DEBUG] Starting fetch of: " + url);

    Terminal.Say("[DEBUG] Making HTTP request...");
    var req = HttpReq.New("GET", url);
    req.SetTimeout(5000);
    var response = HttpReq.Send(req);
    var status = HttpRes.get_Status(response);
    var body = HttpRes.BodyStr(response);

    Terminal.Say("[DEBUG] Response status: " + Fmt.Int(status));
    Terminal.Say("[DEBUG] Response body length: " + Fmt.Int(body.Length()));
    if body.Length() > 200 {
        Terminal.Say("[DEBUG] Response body (first 200 chars): " + body.Substring(0, 200));
    } else {
        Terminal.Say("[DEBUG] Response body: " + body);
    }

    if HttpRes.IsOk(response) {
        Terminal.Say("[DEBUG] Success!");
        return body;
    }

    Terminal.Say("[DEBUG] Request failed with status " + Fmt.Int(status));
    return "";
}
```

### Test Each Layer Separately

Network communication involves multiple layers. Test each one:

1. **Can you reach the host at all?**
```zia
bind Zanna.Terminal as Terminal;

var socket = Tcp.ConnectFor(host, port, 3000);
if socket == null {
    Terminal.Say("Cannot connect to " + host + ":" + port);
    Terminal.Say("Check: Is the server running? Is the address correct?");
    Terminal.Say("Check: Firewall blocking? Network connected?");
}
```

2. **Can you send data?**
```zia
bind Zanna.Terminal as Terminal;

socket.SendStr("test\n");
Terminal.Say("Data sent successfully");
// If this fails, connection might have dropped
```

3. **Can you receive data?**
```zia
bind Zanna.Terminal as Terminal;

var response = socket.RecvLine();
if response == null {
    Terminal.Say("No response from server");
    Terminal.Say("Check: Is server expecting different input format?");
}
```

### Use Simple Test Tools

Before debugging your code, verify the server works with known-good tools:

```zia
// If Http.Get("https://api.example.com") fails...

// Step 1: Can you reach the server at all?
// Use ping or a web browser

// Step 2: Is it your code or the server?
// Try the same URL in a browser
// If browser works but your code doesn't, problem is in your code

// Step 3: Are you sending what you think you're sending?
// Print the exact URL, headers, and body before sending
Terminal.Say("URL: " + url);
Terminal.Say("Headers: " + headers);
Terminal.Say("Body: " + body);
```

### Common Network Error Messages and Their Meanings

| Error | Likely Cause |
|-------|--------------|
| "Connection refused" | Server not running, or wrong port |
| "Connection timed out" | Server unreachable, firewall blocking |
| "Host not found" | DNS failure, hostname wrong |
| "Connection reset" | Server crashed or closed forcefully |
| "Broken pipe" | Tried to write to closed connection |
| "Network unreachable" | No route to destination, no internet |
| "Address already in use" | Port already used by another program |

### The "Works on My Machine" Problem

Network code often works locally but fails when deployed. Common causes:

1. **Firewall differences**: Your machine allows connections that the server blocks
2. **DNS differences**: Your machine resolves names differently
3. **Timing differences**: Network latency varies dramatically
4. **Load differences**: Server handles 1 request fine, fails at 1000

Always test with realistic network conditions before deploying.

---

## A Complete Example: Weather Dashboard

Let's tie everything together with a complete, well-structured networking application:

```zia
module WeatherDashboard;

bind Zanna.Network;
bind Json = Zanna.Data.Json;
bind Clock = Zanna.Time.Clock;
bind Zanna.Terminal as Terminal;
bind Fmt = Zanna.Text.Fmt;
bind Codec = Zanna.Text.Codec;

// Data type for weather information
class CityWeather {
    expose city: String;
    expose temperature: Integer;
    expose conditions: String;
    expose humidity: Integer;
    expose windSpeed: Integer;
    expose lastUpdated: Integer;

    expose func init(city: String, temperature: Integer, conditions: String,
                     humidity: Integer, windSpeed: Integer, lastUpdated: Integer) {
        self.city = city;
        self.temperature = temperature;
        self.conditions = conditions;
        self.humidity = humidity;
        self.windSpeed = windSpeed;
        self.lastUpdated = lastUpdated;
    }
}

// Service class that handles weather API calls
class WeatherService {
    hide apiKey: String;
    hide baseUrl: String;
    hide cache: Map[String, CityWeather];
    hide cacheTimeout: Integer;

    expose func init(apiKey: String) {
        self.apiKey = apiKey;
        self.baseUrl = "https://api.weather.example.com/v1";
        self.cache = new Map[String, CityWeather]();
        self.cacheTimeout = 300000;  // 5 minutes
    }

    // Fetch weather for a city, using cache when possible
    expose func getWeather(city: String) -> CityWeather? {
        // Check cache first
        if self.cache.Has(city) {
            var cached = self.cache.Get(city);

            if cached != null {
                var age = Clock.NowMs() - cached.lastUpdated;

                if age < self.cacheTimeout {
                    Terminal.Say("(Using cached data for " + city + ")");
                    return cached;
                }
            }
        }

        // Fetch from API
        var url = self.baseUrl + "/current?city=" +
                  Codec.UrlEncode(city) + "&key=" + self.apiKey;

        var req = HttpReq.New("GET", url);
        req.SetTimeout(10000);
        var response = HttpReq.Send(req);

        if !HttpRes.IsOk(response) {
            Terminal.Say("API error for " + city + ": " + Fmt.Int(HttpRes.get_Status(response)));

            // Return stale cache if available
            if self.cache.Has(city) {
                Terminal.Say("(Using stale cached data)");
                return self.cache.Get(city);
            }

            return null;
        }

        // This example expects a flat response like:
        // {"temperature":72,"conditions":"Sunny","humidity":45,"windSpeed":8}
        var data = Json.Parse(HttpRes.BodyStr(response));

        var weather = new CityWeather(
            city,
            Json.GetInt(data, "temperature"),
            Json.GetStr(data, "conditions"),
            Json.GetInt(data, "humidity"),
            Json.GetInt(data, "windSpeed"),
            Clock.NowMs()
        );

        // Update cache
        self.cache.Set(city, weather);

        return weather;
    }
}

// Display functions
func displayWeather(weather: CityWeather) {
    Terminal.Say("");
    Terminal.Say("+----------------------------------+");
    Terminal.Say("|  " + padRight(weather.city, 32) + "|");
    Terminal.Say("+----------------------------------+");
    Terminal.Say("|  Temperature: " + padRight(Fmt.Int(weather.temperature) + "F", 17) + "|");
    Terminal.Say("|  Conditions:  " + padRight(weather.conditions, 17) + "|");
    Terminal.Say("|  Humidity:    " + padRight(Fmt.Int(weather.humidity) + "%", 17) + "|");
    Terminal.Say("|  Wind:        " + padRight(Fmt.Int(weather.windSpeed) + " mph", 17) + "|");
    Terminal.Say("+----------------------------------+");
}

func padRight(s: String, width: Integer) -> String {
    var out = s;
    while out.Length() < width {
        out += " ";
    }
    return out;
}

func displayHeader() {
    Terminal.Say("========================================");
    Terminal.Say("         WEATHER DASHBOARD              ");
    Terminal.Say("========================================");
}

// Main program
func start() {
    // In a real app, load this from environment
    var service = new WeatherService("your-api-key-here");

    var cities: List[String] = ["Seattle", "New York", "London", "Tokyo", "Sydney"];

    displayHeader();

    // Fetch weather for all cities
    for city in cities {
        var weather = service.getWeather(city);

        if weather != null {
            displayWeather(weather);
        } else {
            Terminal.Say("");
            Terminal.Say("Could not fetch weather for " + city);
        }
    }

    Terminal.Say("");
    Terminal.Say("Dashboard complete. Data cached for 5 minutes.");
}
```

This example demonstrates:
- HTTP requests with error handling
- JSON parsing
- Caching for efficiency
- Graceful degradation (use stale cache when fresh data unavailable)
- Timeouts
- Clean separation between data fetching and display

---

## The Two Languages

**Zia**
```zia
bind Zanna.Network;
bind Zanna.Terminal as Terminal;

// HTTP request
var response = Http.Get("https://api.example.com/data");
Terminal.Say(response);

// TCP client
var socket = Tcp.Connect("example.com", 80);
socket.SendStr("Hello\n");
var reply = socket.RecvLine();
socket.Close();

// UDP
var udp = Udp.New();
udp.SendToStr("192.168.1.100", 5000, "Hello");
udp.Close();
```

**BASIC-style pseudocode**
```text
DIM response AS HttpResponse
response = HTTP_GET("https://api.example.com/data")
IF response.Ok THEN
    PRINT response.Body
END IF

DIM sock AS TcpSocket
sock = TCP_CONNECT("example.com", 80)
TCP_WRITE sock, "Hello" + CHR$(10)
DIM reply AS STRING
reply = TCP_READLINE(sock)
TCP_CLOSE sock

DIM udp AS UdpSocket
udp = UDP_CREATE()
UDP_SEND udp, "Hello", "192.168.1.100", 5000
UDP_CLOSE udp
```

The current networking runtime is exposed through Zia classes under `Zanna.Network`. The BASIC-style block is conceptual pseudocode, not current BASIC syntax.

---

## Summary

Networking transforms programs from isolated calculators into connected systems. Here's what we covered:

- **The postal analogy**: Network communication is like sending letters --- addresses, routes, delivery guarantees
- **IP addresses and ports**: Addresses identify computers, ports identify programs on those computers
- **TCP vs UDP**: TCP is reliable and ordered (like registered mail), UDP is fast and unreliable (like postcards)
- **Client-server architecture**: Servers wait for and respond to clients
- **HTTP**: The protocol of the web, with methods like GET, POST, PUT, DELETE
- **TCP sockets**: Raw connections for custom protocols
- **UDP sockets**: Fast, connectionless communication for games and real-time data
- **WebSockets**: Real-time bidirectional web communication
- **Error handling**: Networks fail in many ways --- always handle errors
- **Security**: Use HTTPS, validate input, don't trust the network
- **Debugging**: Log everything, test each layer, understand error messages

The ability to make programs communicate opens vast possibilities. Web services, multiplayer games, chat applications, IoT devices, distributed systems --- all become possible when your programs can talk to the world.

---

## Exercises

**Exercise 22.1 (Mimic)**: Modify the weather dashboard to display temperature in Celsius instead of Fahrenheit. Add a helper function to convert between them.

**Exercise 22.2 (Mimic)**: Write a program that fetches a web page and counts how many times a specific word appears. The program should ask the user for the URL and the word to search for.

**Exercise 22.3 (Extend)**: Create a simple HTTP client that can download files and save them locally. Handle large files by reading in chunks rather than all at once.

**Exercise 22.4 (Extend)**: Build an echo server: whatever a client sends, the server sends back. Then modify it to reverse the text before echoing.

**Exercise 22.5 (Create)**: Create a "quote of the day" server. It should maintain a list of quotes and send a random one to each client that connects. Bonus: let administrators add new quotes via a special command.

**Exercise 22.6 (Create)**: Build a simple port scanner. Given a host and a range of ports (e.g., 1-1000), try to connect to each port and report which ones are open (connection succeeds) and which are closed (connection fails).

**Exercise 22.7 (Create)**: Implement a UDP-based "ping pong" game for two players. Each player has a paddle they can move up and down, and a ball bounces between them. Player positions and ball position are sent via UDP.

**Exercise 22.8 (Challenge)**: Create a multiplayer tic-tac-toe game. One player hosts (acts as server), another connects as client. They take turns, and both see the board update in real-time.

**Exercise 22.9 (Challenge)**: Build a simple HTTP server that serves files from a directory. When a client requests `/file.txt`, read `file.txt` from the local directory and return it. Handle missing files with a 404 response. Be careful about security --- don't allow `../` in paths!

**Exercise 22.10 (Challenge)**: Implement a reliable message protocol on top of UDP. Each message should have a sequence number, and the receiver should send acknowledgments. The sender should retransmit messages that aren't acknowledged within a timeout. This is essentially implementing a simplified version of TCP!

---

*We can communicate over networks. But what format should our data take? Next, we explore data formats: JSON, XML, and binary protocols --- the languages that let different programs understand each other.*

*[Continue to Chapter 23: Data Formats](23-data-formats.md)*
