# Phase 1 — Foundations:
Build a working TCP echo server and internalize the five syscalls every network server in existence is built on top of.
## Concepts learnt
### Definitions:
#### File Descriptors
**File descriptor** is a small non‑negative integer that an operating system gives a process when it opens a resource — it acts as a direct handle to read from, write to, or close that resource, whether it's a file, a network socket, or a device. It serves as an index into a process's private table, which the kernel uses to locate the actual object in memory.

**A file descriptor is the universal key for accessing resources.**
- The operating system **assigns** this number to represent any open item.
    - This includes real files, network connections, pipes, and hardware devices.
        - For instance, a call to `read()` or `write()` **requires** a file descriptor to know which resource to use.

**Standard file descriptors are always the same three numbers.**
- The numbers `0`, `1`, and `2` are **reserved** for basic input and output streams.
    - They always represent standard input, standard output, and standard error, respectively.
        - This **guarantees** that every program starts with these three communication channels already open.

**Network sockets are accessed through file descriptors.**
- When you create a socket with `socket()`, the system call **returns** a file descriptor for it.
    - All subsequent operations, like `bind()` or `listen()`, use that same descriptor.
        - This **treats** the network connection just like a file for reading and writing data.

---
**Examples**
- **Creating a socket to listen for connections**When a server program calls `socket()`, the kernel **creates** the endpoint and _returns_ a new file descriptor number for it.
- **Reading data from a client connection**. After `accept()` gives the server a new descriptor, the server can **call** `read()` on it to _receive_ data sent by that specific client.
---
> - A file descriptor is **only** meaningful within the process that owns it; passing the raw integer to a different process has no effect because each process has its own table.
> - The operating system **reuses** the smallest available non‑negative integer when assigning a new descriptor, so closing a file frees up that number for future use.
> - While often called a "handle," the descriptor itself **holds** no data—it is just an integer reference, and the actual resource data is managed completely by the kernel.
#### File Descriptor Table
**File descriptor table** is a private list inside each process that holds pointers to kernel objects — it maps small integer file descriptors to real system resources like open files, sockets, and pipes.Every process has its own **unique** copy of this table, which the operating system uses to track all the resources that process can access.

**The table translates simple numbers into real resources.**
- It **stores** an entry for each open file descriptor a process owns.
- Each entry **points** to a kernel data structure representing an actual file, socket, or device.
- For example, when a process calls `read(fd)`, the OS **looks up** `fd` in this table to find which file to read from.

**The table's size limits how many files a process can open.**
- The OS **imposes** a maximum number of entries per process.
- This **limit** is often set by the system's `ulimit` configuration.
- A server hitting this limit **cannot** accept new connections or open more files.

**Entries are created when a resource is opened.**
- System calls like `open()` or `socket()` **add** a new entry to the table.
- The call **returns** the small integer index (the file descriptor) for that new entry.
- The `close()` system call **removes** the entry and frees the descriptor for reuse.

**Standard streams occupy the first three slots.**
- Descriptors `0`, `1`, and `2` are **pre-assigned** to stdin, stdout, and stderr.
- These entries are **created** automatically when a process starts.
- This **ensures** every program has a basic way to receive input and send output.

**Examples**
**A server creates a socket and gets a new table entry.**When `socket()` runs, it **adds** an entry and _returns_ its file descriptor number.
**A process reading from a file uses the table to find it.**The `read(fd)` call **looks up** the `fd` in the table to _locate_ the correct file data.

> - The table is **per-process**, so a file descriptor number in one process points to a **different** resource than the same number in another process.
> - The table only **holds** pointers; the actual kernel objects remain **separate** and can be shared if multiple descriptors point to the same object.
> - When a process **forks**, the child gets a **copy** of the parent's entire file descriptor table, allowing both to access the same open files.
> - The table is **managed** entirely by the kernel; a user program cannot read or modify its entries directly, only through system calls.

#### Network Socket
**Network socket** is a software object the operating system creates — it acts as a **communication endpoint** that lets a program send and receive data over a network, identified by a unique file descriptor number. The socket **exists** inside the operating system's kernel, not in the user's program.

**A socket is the program's direct link to the network.**
- The operating system **manages** the socket's underlying connection and data flow.
- The program interacts with the socket through a simple **file descriptor** number.

**The socket function creates this endpoint.**
- The `socket()` system call **requests** that the operating system build a new network endpoint.
- It specifies the **communication style**, like TCP for reliable streams.
- For a TCP server, this creates the **listening** endpoint before an address is assigned.

**Sockets enable both sending and receiving data.**
- Once connected, data **travels** through the socket like bytes through a pipe.
- The operating system **handles** the complex packet routing and error checking.
- The program just **reads** from and **writes** to the socket's file descriptor.

**Each accepted client gets its own dedicated socket.**
- The `accept()` call **duplicates** the listening socket for a single client conversation.
- This new socket is **exclusive** to that one client connection.
- The original listening socket **remains** free to accept more incoming clients.

---
> - A network socket is **not** a physical object; it's a software abstraction the kernel maintains, which _consumes_ operating system resources like memory and a table entry.
> - The socket **must** be bound to a specific IP address and port with `bind()` before it can receive any network traffic; an unbound socket cannot _accept_ connections.
> - The socket's file descriptor is just a **reference**; the actual network _logic_—like the TCP three-way handshake—is handled _transparently_ by the operating system's networking stack.
> - A socket is **bidirectional** and can _send_ data immediately after a connection is made, unlike a phone which typically only _receives_ a call initially.
#### IP Address & Port Number
- **IP address** is a unique numeric label assigned to every device on a network. It contains four numbers separated by dots, like `192.168.1.10`, which **identifies** a single computer so data can find it.
- **Port number** is a numeric identifier that a computer assigns to a specific software process — it allows a single machine to run multiple network services at the same time by giving each one a unique channel, like different apartment numbers in the same building.
#### Socket & Port differences
| **Feature**     | **Port**                                                                               | **Socket**                                                                          |
| --------------- | -------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| **What is it?** | A 16-bit integer number (e.g., `80`, `443`, `22`).                                     | An OS-level software object (represented as a file descriptor in Linux).            |
| **Composition** | Just a number.                                                                         | A combination of an IP Address, a Port, and a Protocol (TCP/UDP).                   |
| **Purpose**     | To multiplex network traffic so one computer can run multiple services simultaneously. | To actually send, receive, and manage the stream of data over the network.          |
| **Lifespan**    | Exists as a standardized concept whether a connection is active or not.                | Created dynamically when a program wants to communicate, and destroyed when closed. |
| **Example**     | `443` (Standard port for HTTPS)                                                        | `192.168.1.10:5000` connected via TCP to `10.0.0.10:443`                            |
#### TCP vs UDP
TCP (Transmission Control Protocol) is a connection-oriented protocol that guarantees perfect data delivery, while UDP (User Datagram Protocol) is a connectionless protocol that sacrifices reliability to achieve maximum speed.
Think of TCP as a registered mail service where the recipient must sign for every package (and missing packages are resent). UDP is like throwing a handful of postcards into a mailbox—they usually get there quickly, but if one gets lost in transit, it's gone forever.

|**Feature**|**TCP (Transmission Control Protocol)**|**UDP (User Datagram Protocol)**|
|---|---|---|
|**Connection Type**|Connection-oriented. Requires a "3-way handshake" before sending data.|Connectionless. Starts blasting data immediately.|
|**Reliability**|**High.** Guarantees delivery. If a packet drops, TCP retransmits it.|**Low.** "Best effort." No guarantee packets will arrive.|
|**Data Ordering**|Guarantees packets are reassembled in the exact original order.|Packets may arrive out of order, and UDP doesn't fix it.|
|**Speed & Overhead**|Slower. The constant checking, acknowledging, and ordering creates overhead.|Very Fast. Almost zero overhead because it doesn't wait for acknowledgments.|
|**Header Size**|20 to 60 bytes|8 bytes|

**When is TCP Used?**
You use TCP when data accuracy is non-negotiable, even if it takes a few milliseconds longer. If a single packet drops while downloading a file, the whole file corrupts.

- **Web Browsing:** HTTP/HTTPS

- **File Transfers:** FTP, downloading updates

- **Email:** SMTP, IMAP

- **Secure Shell:** SSH

**When is UDP Used?**

You use UDP for real-time applications where a dropped packet just causes a momentary glitch, but waiting for a retransmission would ruin the experience (like a lag spike in a game).

- **Live Video/Audio Streaming:** Netflix, YouTube, Spotify

- **Voice over IP (VoIP):** Zoom, Discord, Skype calls

- **Online Multiplayer Gaming:** Fast-paced shooters where the latest positional data matters more than past data.

- **DNS:** Translating domain names to IP addresses.
#### Socket Option
A socket option is a setting that modifies the behavior of a socket in network programming. Ex: SO_REUSEPORT
#### SO_REUSEPORT 
`SO_REUSEPORT` is a socket option that allows multiple sockets to bind to the exact same port and IP address. This enables load‑balancing at the OS kernel level incoming connections are distributed among all listening sockets.
### Input/Output:
- A blocking I/O operation pauses your program until the request is complete. 
- For example, `accept()` blocks until a client connects. `read()` blocks until data arrives.
- It's the simplest model: your code runs step‑by‑step, waiting as needed.
- The downside: your server can only handle one client at a time while blocked.
- This is why real servers use more advanced patterns like non‑blocking I/O, multiprocessing, or event loops.

|**Feature**|**Multiprocessing**|**Naive Non-Blocking I/O**|**Event Loop (epoll/kqueue)**|
|---|---|---|---|
|**Max Concurrent Sockets**|Low (~1,000s)|Moderate (~10,000s)|**Extreme (100,000+)**|
|**Memory Overhead**|High (MBs per connection)|Low|**Very Low (KBs per connection)**|
|**CPU Efficiency**|Low at scale (context switching)|Terrible if busy-waiting|**High (OS notifies only on activity)**|
|**Core Architecture**|Multi-process|Single/Multi-thread|Single-thread event loop per CPU core|
|**Typical Use Case**|Legacy Unix daemons, legacy CGI|Low-level driver design|**High-throughput API servers, proxies, chat systems**|
#### Non-Blocking I/O
In **non-blocking mode**, the operating system never puts your thread to sleep. I/O calls are forced to return immediately, whether they succeed or not.

**How to Enable It (`O_NONBLOCK` & `fcntl()`)**

Activate non-blocking mode by applying the `O_NONBLOCK` status flag to a file descriptor (like a network socket) using the `fcntl()` (file control) function.
**Best Practice (The "Get-Then-Set" Method):** Don't just overwrite the flags. Fetch the existing ones, add `O_NONBLOCK` via bitwise OR, and set them back so you don't erase other important settings:

```C++
int flags = fcntl(sockfd, F_GETFL, 0); // 1. Get current flags
fcntl(sockfd, F_SETFL, flags | O_NONBLOCK); // 2. Add O_NONBLOCK and set
```

**How I/O Functions Behave**

Once a socket is non-blocking, standard functions (`read()`, `write()`, `accept()`, `recv()`, `send()`) change their behavior:

- **If ready:** They execute instantly (e.g., returning the data read or the new client socket).
    
- **If NOT ready:** They return `-1` immediately and set `errno` to **`EAGAIN`** or **`EWOULDBLOCK`**.
    
- **The Golden Rule:** `EAGAIN`/`EWOULDBLOCK` simply mean "the buffer is empty/full, try again later." Your code must handle this gracefully as a normal status update, not a fatal crash.

- **The Catch (Busy-Waiting):** Because non-blocking calls return instantly, you might be tempted to put `read()` inside a `while(true)` loop to check for data. This creates a "busy-wait" that will peg your CPU at 100%.
    
- **The Solution (Event Notification):** To fix this, modern OSs provide notification interfaces like **`select`**, **`poll`**, or Linux's high-performance **`epoll`**. These allow your thread to sleep safely and efficiently until the OS wakes it up to announce exactly which socket has data ready.
#### Multiprocessing
- **How it works:** A master server process accepts incoming connections and calls `fork()` to create a complete copy of itself (a child process) for every new client. Each process gets its own dedicated memory space, file descriptors, and execution thread managed directly by the operating system scheduler.
- **Why it was used:** It is incredibly simple to write (sequential blocking code) and crash-isolated. If a client sends malicious data that crashes a child process, only that single child dies—the main server and other clients remain running. It also scales across multiple CPU cores automatically.
- **Why it breaks:** OS processes are heavy. Every process consumes dedicated stack memory, and switching between thousands of processes forces the CPU to constantly swap cache and address spaces (context switching), causing system performance to collapse.
#### Event Loops
- **How it works:** A single thread runs a continuous loop that acts as an event coordinator. Instead of managing threads or processes, the event loop registers thousands of non-blocking sockets with the OS kernel's event multiplexer (`epoll` on Linux, `kqueue` on macOS/BSD). When network packets arrive, the OS alerts the event loop, which executes short callback functions or resumes coroutines (`async`/`await`) to process the data.
- **The Strength:** Extremely low memory overhead. Because you aren't creating thousands of threads or processes, 100,000 open sockets only cost the memory required for the underlying TCP socket buffers in the kernel.
- **The Vulnerability:** Since a single thread processes all callbacks sequentially, **any CPU-heavy work (like hashing passwords, parsing large JSON files, or image processing) blocks the loop**, causing all other 99,999 clients to wait. CPU-heavy tasks must be offloaded to a separate worker thread pool.
### TCP Three‑Way Handshake: The Reliable Introduction
This is the process a client and server use to establish a TCP connection before any data is sent. It ensures both sides are ready.

Step 1: SYN (Synchronize)
- The client sends a packet with the `SYN` flag set, meaning "I want to start a connection, and my initial sequence number is X."

Step 2: SYN‑ACK (Synchronize‑Acknowledge)
- The server replies with a packet that has both `SYN` and `ACK` flags set.
- It means: "I acknowledge your sequence number X+1, and my initial sequence number is Y."

Step 3: ACK (Acknowledge)
- The client sends an `ACK` packet back, acknowledging the server's sequence number Y+1.
- At this point, the connection is established and data transfer can begin.

> Why three steps? It guarantees both sides know the other is alive and agrees on how to number the bytes they'll send, which is crucial for reliability and in‑order delivery.  (**In‑order delivery** is a guarantee that a network protocol makes that the receiving application will get all data bytes in the exact same sequence the sender originally transmitted them.).
### Byte-Ordering
When a computer stores a number, it doesn't always fit into a single memory slot. A 16-bit number like the hexadecimal `0x1234` requires two bytes of memory: the "high" byte (`0x12`) and the "low" byte (`0x34`).

Different computer chips have different rules for which piece goes into memory first:
- **Big-Endian:** Reads naturally left-to-right. The most important part (the big end, `0x12`) goes into the first memory slot. The `0x34` goes into the next slot.
- **Little-Endian:** Reads backward. The least important part (the little end, `0x34`) goes into the first memory slot, and `0x12` goes into the next. _This is how almost all modern personal computers (x86/x64 Intel and AMD chips) work._

When computers connect to the internet, they need a universal standard so they don't misread each other's data. Decades ago, the creators of the internet decided that **Network Byte Order will always be Big-Endian**.

This creates a conflict: your computer wants to process data backward (Little-Endian), but the internet demands it forward (Big-Endian). If you send a port number like `0x1234` directly from your Intel CPU to the network, the network will read it backward as `0x3412`, sending your data to the completely wrong place.

**The solution:**
To bridge this gap without you having to manually write code to flip bytes, programming languages provide built-in translation functions. The names look like alphabet soup, but they follow a strict naming formula:
- **h** = **H**ost (your computer's CPU format)
- **n** = **N**etwork (the internet's Big-Endian format)
- **s** = **S**hort (a 16-bit number, usually used for **Ports**)
- **l** = **L**ong (a 32-bit number, usually used for **IP Addresses**)

**Sending Data Out:**
- `htons()` (Host to Network Short): Takes your 16-bit port number and flips the bytes if your CPU is Little-Endian so the network can understand it.
- `htonl()` (Host to Network Long): Does the same thing, but for 32-bit IP addresses.

**Receiving Data In:**
- `ntohs()` (Network to Host Short): Takes the network's 16-bit Big-Endian port number and flips it back so your computer's CPU can read it properly.
- `ntohl()` (Network to Host Long): Does the same thing for incoming 32-bit addresses.

If your computer _happens_ to be a Big-Endian machine, these functions are smart enough to do absolutely nothing. You use them on **every** network integer so your code works perfectly no matter what hardware it runs on.
### TCP Complete Lifecycle
To accept network traffic, a TCP server must move through a strict sequence of state transitions managed by the OS kernel:

```
      socket()      --> Create the socket handle
         │
      bind()        --> Assign IP address + Port
         │
      listen()      --> Enable passive listening (Kernel manages TCP Handshakes)
         │
      accept()      --> Block until client connects -> Returns new client_fd
      ┌──┴────────┐
   recv()      send()  --> Read/Write data with client
      └──┬────────┘
      close()       --> Tear down TCP connection (FIN packet)
```
#### 1. socket() – Obtaining a Kernel Handle

```C++
int sockfd = socket(AF_INET, SOCK_STREAM, 0);
```
- **What it does:** Requests the kernel to allocate network protocol state structures in RAM and return an integer **File Descriptor** (`sockfd`) to reference them. 
- When you create a network socket, you are essentially asking your operating system to open a communication channel. To do this, the system needs to know two fundamental things: **where** the data is going (the network type) and **how** it should be delivered (the protocol). `AF_INET` and `SOCK_STREAM` answer these two exact questions.
- **Key Arguments:**
    - `AF_INET`: Address Family IPv4 - Internet Protocol version 4 (use `AF_INET6` for IPv6).
    - `SOCK_STREAM`: Specifies reliable, sequential byte streams (**TCP**). Use `SOCK_DGRAM` for **UDP**.
- **Return Value:** An integer descriptor ($\ge 0$) or `-1` if the kernel runs out of file descriptors.
#### 2. bind() – Claiming an IP and Port

```C++
struct sockaddr_in addr;
addr.sin_family = AF_INET;
addr.sin_port = htons(8080);                   // Converts 8080 to Big-Endian
inet_pton(AF_INET, "0.0.0.0", &addr.sin_addr); // Bind to all local network cards

bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
```

- **What it does:** Attaches your process to a specific port and IP address on the host OS.
- **`struct sockaddr_in addr;`** Creates a structured container to hold IPv4 address information.
- **`addr.sin_family = AF_INET;`** Specifies that this address is for IPv4.
- **`addr.sin_port = htons(8080);`** Sets the port to 8080. `htons()` ensures the number is formatted correctly for the network (Network Byte Order/Big-Endian), regardless of how your specific CPU stores numbers.
- **`inet_pton(AF_INET, "0.0.0.0", ...);`** Converts the text IP `"0.0.0.0"` into a binary format. `0.0.0.0` is a special address meaning "bind to all available local IP addresses."
- **`server_addr.sin_addr.s_addr = htonl(INADDR_ANY);`** It configures the server socket to listen for incoming connections on all of the computer's available network interfaces (such as Wi-Fi, Ethernet, and localhost) simultaneously. It is slightly faster and requires less code than writing out `"0.0.0.0"` and using `inet_pton` to convert the string into binary.
- **`bind(...);`** Actually applies this configuration, permanently linking the `sockfd` socket to this IP and port combination.
- **Key Details:**
    - `0.0.0.0` (`INADDR_ANY`): Instructs the kernel to accept traffic arriving on **any** network interface (Ethernet, Wi-Fi, Loopback/localhost).    
    - `htons(8080)`: Converts the integer `8080` into **Network Byte Order (Big-Endian)** so the kernel registers the correct port.
#### 3. listen() – Activating Passive Mode
```C++
listen(sockfd, 128); // Backlog queue size
```

- **What it does:** Converts the socket from active (outbound) to passive (inbound), allowing it to receive incoming connections.
- **The Backlog Queue:** When a client sends a `SYN` packet to start a TCP 3-way handshake, the OS kernel completes the handshake **in the background**. The second argument (`128`) defines how many fully established connections can sit in the kernel's connection queue waiting for your code to collect them.
#### 4. accept() – Extracting Established Connections

```C++
struct sockaddr_in client_addr;
socklen_t addr_len = sizeof(client_addr);

int client_fd = accept(sockfd, (struct sockaddr*)&client_addr, &addr_len);
```

- **What it does:** Extracts the first pending connection request from the listening socket's queue, establishes a connection, and creates a brand new socket dedicated specifically to communicating with that client.

- **`struct sockaddr_in client_addr;`** Creates an empty structured container to hold the connecting client's IPv4 address and port information.
 
- **`socklen_t addr_len = sizeof(client_addr);`** Calculates the size of the address container in bytes. The `accept` function needs this to ensure it doesn't write past the bounds of the allocated memory.

- **`int client_fd = accept(...);`** Pauses (blocks) your program until a client attempts to connect. Upon success, it creates a brand new socket for this specific connection and returns its file descriptor.

	- **`sockfd`** The existing, passive socket that has already been configured to listen for incoming connections.
	- **`(struct sockaddr*)&client_addr`** Casts the pointer of your IPv4-specific structure into a generic socket address pointer, allowing the underlying C function to populate it with the connecting client's IP and port details.
	- **`&addr_len`** Passes a pointer to the length variable. This acts as an "value-result" parameter: you tell the kernel the maximum size of your buffer, and the kernel overwrites the variable to tell you exactly how many bytes it actually used.
  

**Key Details:**

- **`client_fd` vs `sockfd`:** `accept()` creates a two-socket system. It returns a _new_ socket (`client_fd`) used purely for sending and receiving data with this specific client. The original socket (`sockfd`) is untouched and goes right back to listening for new, incoming connections.  
- **Pass-by-reference (`&`):** By passing memory addresses (`&client_addr` and `&addr_len`), you allow the operating system kernel to actively push the remote client's network information directly into your program's variables so you know who just connected to you.

#### 5. read() / recv() – Receiving Bytes

```C++
// Standard Unix Read
ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));

// Network-Specific Recv
ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), MSG_PEEK);
```

- **What it does:** Retrieves incoming data sent by a connected client and stores it in a local memory buffer. It highlights the difference between the universal Unix file reading method (`read`) and the network-specific method (`recv`) which allows for advanced networking flags.

- **`ssize_t bytes_read = ...;`** Declares a variable to hold the result of the read operation. `ssize_t` is a signed integer, meaning it can return a positive number (bytes successfully read), `0` (client gracefully disconnected), or `-1` (an error occurred).

- **`read(client_fd, buffer, sizeof(buffer));`** The standard Unix way to read data. Because Unix treats "everything as a file," this function interacts with the network socket exactly as if it were reading text from a regular text file on your hard drive.

- **`recv(client_fd, buffer, sizeof(buffer), MSG_PEEK);`** The network-specific way to read data. It functions identically to `read`, but includes a fourth parameter that allows you to pass special network-level instructions (flags) to the operating system kernel.

- **`buffer`** The local memory space (usually a `char` array) where the kernel will deposit the incoming data.

- **`sizeof(buffer)`** Tells the function the absolute maximum number of bytes it is allowed to copy into your buffer. This is critical for preventing buffer overflow vulnerabilities.

- **`MSG_PEEK`** A specific `recv` flag. It allows you to "peek" at the incoming data by copying it into your buffer _without_ removing it from the operating system's network queue. The next time you call `read` or `recv`, you will receive this exact same data again.

**Key Details:**

- **Interchangeability:** If you pass `0` as the flag to `recv()` (e.g., `recv(..., 0)`), it behaves 100% identically to `read()`.
    
- **Scope:** `read()` works on almost any file descriptor (files, pipes, terminals, sockets). `recv()` is strictly for sockets; it will fail if you try to use it to read a standard text file.
    
    c
- **The Zero Return:** Checking if `bytes_read == 0` is the standard way to detect that the client has intentionally closed their end of the connection (an EOF, or End of File, signal).
#### 6. write() / send() – Transmitting Bytes

```C++
// Standard Unix Write
write(client_fd, buffer, bytes_to_send);

// Network-Specific Send
send(client_fd, buffer, bytes_to_send, MSG_NOSIGNAL);
```

**What it does:** Transmits data from your program's memory across the network to a connected client. It contrasts the universal Unix file writing method (`write`) with the network-specific method (`send`) which allows for advanced control flags.

- **`write(client_fd, buffer, bytes_to_send);`** The standard Unix way to transmit data. Because Unix treats "everything as a file," this function writes data to the network socket exactly as if it were saving text to a regular file on your hard drive.

- **`send(client_fd, buffer, bytes_to_send, MSG_NOSIGNAL);`** The network-specific way to transmit data. It functions identically to `write`, but includes a fourth parameter that allows you to pass special network-level instructions (flags) to the operating system kernel.

- **`buffer`** The local memory space (pointer or array) containing the actual data you want to transmit.

- **`bytes_to_send`** The exact number of bytes from your buffer that you want to push onto the network.

- **`MSG_NOSIGNAL`** A specific `send` flag used for crash prevention. If a client unexpectedly drops the connection, a standard `write` will trigger a `SIGPIPE` signal from the OS, which instantly crashes your entire program by default. `MSG_NOSIGNAL` suppresses this fatal signal, forcing the function to instead safely return `-1` so your code can handle the error gracefully.

**Key Details:**

- **Interchangeability:** Just like `read` and `recv`, if you pass `0` as the flag to `send()` (e.g., `send(..., 0)`), it behaves 100% identically to `write()`.
    
- **Scope:** `write()` works on almost any file descriptor (files, pipes, terminals, sockets). `send()` is strictly for sockets; it will fail if you try to use it to write to a standard text file.
    
- **Partial Sends:** Neither function guarantees all your data will be sent in a single burst. Both return an integer representing the _actual_ number of bytes successfully pushed to the network queue. If the return value is less than `bytes_to_send`, you must use a loop to send the remaining data
#### 7. close() – Releasing the Socket

```C++
close(client_fd);
```
- **What it does:** Closes the file descriptor, frees memory buffers in the kernel, and initiates the standard **TCP 4-Way Handshake teardown** (sends a `FIN` packet to the peer).
### String vs. Binary Helpers: inet_pton() & inet_ntop()
Computers process IP addresses as 32-bit (IPv4) or 128-bit (IPv6) integers, but humans write them as text strings (`"192.168.1.1"`).

| **Function**  | **Direction**                   | **Full Name**                       | **Input → Output**                         |
| ------------- | ------------------------------- | ----------------------------------- | ------------------------------------------ |
| `inet_pton()` | **String $\rightarrow$ Binary** | **P**resentation **T**o **N**etwork | `"192.168.1.1"` $\rightarrow$ `0xC0A80101` |
| `inet_ntop()` | **Binary $\rightarrow$ String** | **N**etwork **T**o **P**resentation | `0xC0A80101` $\rightarrow$ `"192.168.1.1"` |

```C++
// Example String to Binary Conversion
struct in_addr ip_binary;
inet_pton(AF_INET, "192.168.1.100", &ip_binary);

// Example Binary to String Conversion
char ip_string[INET_ADDRSTRLEN];
inet_ntop(AF_INET, &ip_binary, ip_string, INET_ADDRSTRLEN);
```
### epoll
**Epoll** is a highly efficient I/O event notification facility built into the Linux kernel. It acts as a traffic controller for network applications, allowing a single thread to monitor thousands of network connections simultaneously to see if any of them are ready to send or receive data.

Before epoll, Linux relied on `select` and `poll`, which required the kernel to linearly scan every active connection to find the ones with new data—an operation that severely degrades in performance as connections scale.
#### What is it Used For?

Epoll is designed to solve the **C10K problem**—the challenge of handling 10,000+ concurrent connections on a single server without exhausting system resources.

Instead of creating a heavy, memory-intensive OS thread for every user, servers can use one thread to handle thousands of connections asynchronously. If you use **Nginx**, **Redis**, **HAProxy**, or run JavaScript on **Node.js** (via the `libuv` library) on a Linux machine, epoll is the underlying engine keeping them fast.
#### How it works?

`epoll` relies on two distinct data structures in the kernel to achieve massive scalability:

**1. The Red-Black Tree (The Watchlist)**

- **Its Role:** It stores every single file descriptor that you have asked `epoll` to monitor.
    
- **Why a Red-Black Tree?** Red-Black trees are self-balancing binary search trees. When an application adds, modifies, or deletes a monitored file descriptor (using `epoll_ctl`), the kernel can execute this in **$O(\log N)$** time. This guarantees fast lookups and updates even when a web server is juggling hundreds of thousands of concurrent connections.
    

**2. The Ready List (The Action List)**

- **Its Role:** A doubly-linked list that contains **only** the FDs that currently have pending I/O events (e.g., data has arrived).
    
- **How it works:** When a network packet arrives, a hardware interrupt fires and triggers a kernel callback. This callback automatically copies a reference to the active FD from the Red-Black Tree directly into the Ready List.
    
#### The Workflow Summary
To interact with epoll in C/C++, you use three system calls:

1. `epoll_create1(int flags)`: Creates an epoll instance in the kernel and returns a file descriptor pointing to it. Setting up an empty Red-Black Tree and an empty ready list
    
2. `epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)`: Adds, modifies, or removes FDs in the **Red-Black tree**. 
	- **`epfd`**: The `epoll` instance handle.
    - **`op`**: The action to perform (`EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`, or `EPOLL_CTL_DEL`).
    - **`fd`**: The target socket or file descriptor.
    - **`event`**: Specifies the I/O events to watch (e.g., `EPOLLIN` for read, `EPOLLOUT` for write) and attached user context data.
    - ```C++
      struct epoll_event { 
	      uint32_t events; /* Epoll events */ 
	      epoll_data_t data; /* User data variable */ 
      };
      // `events`: A bitmask that tells the kernel exactly what to watch for (like `EPOLLIN` for incoming data) or reports what just happened.
      // `data`: A custom nametag (usually the socket's file descriptor) that the kernel hands back to you untouched, so you know _which_ connection triggered the event.
      ```
    **Operations (Executed in $O(\log N)$ time)**
    - **`EPOLL_CTL_ADD`**: Inserts `fd` into the Red-Black tree and registers kernel callbacks to push events to the Ready List.
    - **`EPOLL_CTL_MOD`**: Updates event triggers or attached user data for an existing `fd`.
    - **`EPOLL_CTL_DEL`**: Removes `fd` from the Red-Black tree and detaches its kernel callbacks.
    
3. `epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)`: The blocking call. Checks the **Ready List**. If the list has items, it immediately returns them to the application. If the list is empty, it sleeps until a kernel callback wakes it up. Because it only checks a pre-populated list, `epoll_wait()` operates in **$O(1)$** time. 
	-  **Buffer Limit (`maxevents`):** You pass an array buffer and a `maxevents` inttheeger to `epoll_wait()`. The kernel copies up to `maxevents` ready entries from the Ready List into user space.
	-  **Overflow Behavior:** If more file descriptors are ready than `maxevents`, `epoll_wait()` fills up to `maxevents`, returns that count, and leaves the remaining ready events in the kernel Ready List to be fetched in subsequent `epoll_wait()` calls.
	- The server simply loops through the returned array, looks at the `data.fd` to see _who_ is ready, looks at the `events` bitmask to see _what_ they are ready for, and executes the appropriate operation.
    
#### Integrating Epoll into a TCP Lifecycle

To build a non-blocking TCP server, epoll sits between your standard socket setup and your read/write logic. The general flow is:

1. Create, bind, and listen on a socket.
    
2. Create an epoll instance (`epoll_create1`).
    
3. Add the listening socket to the epoll instance (`epoll_ctl`).
    
4. Enter an event loop that blocks on `epoll_wait`.
    
5. When `epoll_wait` returns, accept new connections or read/write data for existing ones based on which sockets are ready.
6. Common `events` flags include:

- `EPOLLIN`: data is available to read
- `EPOLLOUT`: socket can accept writes
- `EPOLLERR`: error occurred
- `EPOLLHUP`: connection was closed
- `EPOLLRDHUP`: peer closed its writing side / no more data will be received
- `EPOLLET`: edge-triggered mode

### OS Resource Limits & Syscalls
Operating systems enforce resource limits on every process to prevent a single buggy or malicious application from consuming all available memory, CPU time, or file handles and crashing the entire machine.
#### RLIMIT_NOFILE
Every open file, pipe, and network socket requires a slot in the process’s **File Descriptor (FD) Table**.
- **The Problem:** By default, Linux restricts a single process to **1,024 open file descriptors** (`ulimit -n`). If a TCP server attempts to accept client #1025, `accept()` fails with `EMFILE` (Too many open files), even if the machine has 128 GB of free RAM.
- **The Solution:** Production servers (like Nginx or custom TCP engines) must raise this cap to $100,000+$ at startup before accepting network connections.
#### Soft Limits vs. Hard Limits
Every OS resource limit consists of two separate bounds set inside the kernel:

|**Limit Type**|**Who Controls It**|**Purpose**|
|---|---|---|
|**Soft Limit**|The Process|The **actual active limit** enforced by the kernel. A process can raise its own soft limit up to the value of the hard limit at any time.|
|**Hard Limit**|System Administrator (`root`)|The **absolute ceiling** for the soft limit. Only root processes can increase the hard limit.|
##### Soft Limits (The "Warning Zone"):

**Definition:** A threshold that can be temporarily exceeded but triggers a warning or temporary restriction.

- **Behavior:** Does not immediately stop the system or user.
    
- **Action taken:** Sends alert emails, throttles performance (slows things down), or starts a "grace period" countdown.
    
- **Consequence:** If the limit isn't respected before the grace period ends, it usually turns into a hard limit block.

##### Hard Limits (The "Absolute Ceiling")

**Definition:** The absolute maximum capacity that cannot be exceeded under any circumstances.

- **Behavior:** Strictly blocks any further usage the moment it is reached.
    
- **Action taken:** Rejects requests, displays error messages, or causes tasks to fail.
    
- **Consequence:** User must free up resources or purchase more capacity to continue.

##### How They Work Together

Systems pair these limits so users have time to fix issues _before_ their work is interrupted.

| **Feature**               | **Soft Limit**                         | **Hard Limit**                         |
| ------------------------- | -------------------------------------- | -------------------------------------- |
| **Exceedable?**           | Yes (temporarily)                      | No (never)                             |
| **System Response**       | Warnings, alerts, grace periods        | Total block, rejected requests, errors |
| **Cloud Storage Example** | Hit 100GB: Get a "Storage Full" email  | Hit 110GB: Cannot save new files       |
| **Credit Card Example**   | Hit $500: Budget notification triggers | Hit $1,000: Card declines at register  |


#### Controlling Limits Programmatically: getrlimit() & setrlimit()
Rather than relying on shell configurations (`ulimit -n`), production servers configure their required resources on startup using POSIX system calls:
```C++
#include <sys/resource.h>

struct rlimit rl;

// 1. Query current limits for max open file descriptors
getrlimit(RLIMIT_NOFILE, &rl);

// 2. Increase the soft limit to match the maximum allowed hard limit
rl.rlim_cur = rl.rlim_max; 

// 3. Apply the updated limit back to the kernel
setrlimit(RLIMIT_NOFILE, &rl);
```

#### Essential Resource Limits Summary

| **Resource Limit Constant** | **Controls**                                | **Why Network Servers Adjust It**                                             |
| --------------------------- | ------------------------------------------- | ----------------------------------------------------------------------------- |
| **`RLIMIT_NOFILE`**         | Max open file descriptors (files + sockets) | Raised to allow thousands of simultaneous client socket connections.          |
| **`RLIMIT_STACK`**          | Maximum stack memory size per thread        | Lowered slightly if spawning thousands of threads to conserve virtual memory. |
| **`RLIMIT_AS`**             | Maximum Virtual Memory (Address Space)      | Prevents memory-leak bugs from consuming all available RAM/swap space.        |
| **`RLIMIT_NPROC`**          | Max processes/threads owned by user         | Checked to ensure worker-process pools or thread pools can be spawned.        |

## Implementation
### CMake
**CMake** is an open-source, cross-platform build system generator that reads configuration scripts (`CMakeLists.txt`) to produce native build files (like Makefiles, Ninja scripts, or Visual Studio solutions) for compiling software.
### Opt
```C++
int opt = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```
- lets the server reuse the same port quickly after a restart or crash
- helps avoid:
    - `Address already in use`
    - bind failures when the kernel still has the port in a recent state
- [SO_REUSEADDR](vscode-file://vscode-app/usr/share/code/resources/app/out/vs/code/electron-browser/workbench/workbench.html) = allow reusing the local address/port
- [SOL_SOCKET](vscode-file://vscode-app/usr/share/code/resources/app/out/vs/code/electron-browser/workbench/workbench.html) = socket-level option
- [opt = 1](vscode-file://vscode-app/usr/share/code/resources/app/out/vs/code/electron-browser/workbench/workbench.html) = enable the option
- [sizeof(opt)](vscode-file://vscode-app/usr/share/code/resources/app/out/vs/code/electron-browser/workbench/workbench.html) = size of the option value
When you run the server repeatedly, the OS may keep the port reserved briefly after shutdown. This option makes local testing smoother and prevents immediate bind failures.
### Graceful Shutdown
When you run a server in the terminal and press `Ctrl+C`, the operating system sends an interrupt signal (called `SIGINT`) to the process. By default, this immediately kills your program, which can leave sockets hanging open or cause data loss. This code intercepts that signal to shut down cleanly instead.

```C++
#include <csignal>
volatile sig_atomic_t shutdown_requested = false;

void handle_sigint(int) {
    shutdown_requested = true;
}

// registering the signal
signal(SIGINT, handle_sigint);


// modified accept loop
while (server_fd != -1 && !shutdown_requested) {
    // accept client
}
```

- **The Flag (`volatile sig_atomic_t`):** This creates a safe, interrupt-proof variable that the program checks to see if it should shut down. `volatile` ensures the program checks the actual memory every time rather than a cached version.
    
- **The Intercept (`signal` & `handle_sigint`):** Instead of letting `Ctrl+C` kill your server instantly, you tell the operating system to run your custom function, which simply flips the flag to `true`.
    
- **The Loop:** Your server's main `while` loop constantly checks that flag. If it turns `true`, the loop stops, allowing your program to clean up sockets and exit normally.
    
- **The Error:** Because your server is likely paused waiting for a client at `accept()`, `Ctrl+C` violently interrupts that pause. This causes `accept()` to fail and throw a harmless error just before the loop exits.
### Epoll integration

In this TCP server, `epoll_event` is how the server asks Linux:

> “Watch these sockets and tell me when one needs attention.”

1. The server creates a listening socket: In `TCPServer::setup()`:
```cpp
server_fd = socket(AF_INET, SOCK_STREAM, 0);
bind(server_fd, ...);
listen(server_fd, backlog);
```
`server_fd` is the socket that waits for new clients. It is not used to exchange client data.

2. The listening socket is registered with epoll: In `accept_loop()`:
```cpp
loop.add_fd(server_fd, EPOLLIN);
```
Inside `EpollLoop::add_fd()`:

```cpp
epoll_event event{};
event.events = EPOLLIN;
event.data.fd = fd;

epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
```

This creates an `epoll_event` saying:
- Watch server_fd.
- Notify me when it is readable.
- For a listening TCP socket, “readable” means: At least one client is waiting to be accepted. It does not mean application data is available.

3. `epoll_wait()` waits for activity: The event loop has an array:

```cpp
epoll_event events[64];
```

Then:

```cpp
int event_count = epoll_wait(epoll_fd, events, 64, -1);
```

Linux pauses here until one or more registered sockets are ready. It fills the array with events that actually happened.

There are two uses of `epoll_event`:
- Before epoll_wait():  what should I monitor?
- After epoll_wait():   what became ready?

#### A new client connects

Suppose a client connects. Linux marks `server_fd` as ready and returns something like:

```cpp
events[0].data.fd == server_fd
events[0].events  == EPOLLIN
```

The callback calls:

```cpp
handle_event(loop, server_fd, EPOLLIN);
```

Then this condition matches:

```cpp
if (fd == server_fd) {
    accept_clients(loop);
    return;
}
```

`accept_clients()` calls `accept()` and gets a new descriptor, for example:

```text
server_fd = 3
client_fd = 5
```

After accepting the client:

```cpp
loop.add_fd(client_fd, EPOLLIN);
```

That creates another event:

```text
Watch client_fd.
Notify me when client_fd has data to read.
```

Now epoll watches both:

```text
fd 3: new connections
fd 5: data from client 1
fd 6: data from client 2
...
```

#### The client sends data

When client `fd 5` sends data, `epoll_wait()` returns:

```cpp
events[index].data.fd == 5
events[index].events  == EPOLLIN
```

The server calls:

```cpp
handle_event(loop, 5, EPOLLIN);
```

Since `5 != server_fd`, it reaches:

```cpp
if (events & EPOLLIN) {
    handle_client(loop, fd);
}
```

Then `handle_client()` reads and echoes the data:

```cpp
read(client_fd, buffer, sizeof(buffer));
send(client_fd, buffer, bytes_read, MSG_NOSIGNAL);
```

#### The important idea
`epoll_event` does not contain the client message itself. It only says:
- Which file descriptor is ready?
- What kind of activity occurred?
In this project:
```cpp
event.data.fd
```

identifies the socket, while:

```cpp
event.events
```

contains flags such as `EPOLLIN`, `EPOLLERR`, or `EPOLLHUP`. The actual bytes are read later by `read()` in `handle_client()`.


# Temp
Assume:

```text
epoll_fd  = 4
server_fd = 3
client fds = 5 through 14
```

The numbers are illustrative.

## Initial setup

`start()` performs:

```cpp
socket()
bind()
listen()
set_nonblocking(server_fd)
```

Then `accept_loop()` creates the epoll instance and registers the listening socket:

```cpp
epoll_fd = epoll_create1(0);

epoll_event event{};
event.events = EPOLLIN;
event.data.fd = server_fd;  // 3

epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);
```

The kernel now watches:

```text
epoll instance 4:
    fd 3 -> EPOLLIN
```

For a listening socket, `EPOLLIN` means:

> One or more completed client connections are waiting for `accept()`.

Then the server waits:

```cpp
epoll_wait(4, events, 64, -1);
```

## Ten clients connect

Suppose ten clients connect around the same time. The kernel completes their TCP handshakes and places them in the listening socket's accept queue:

```text
server_fd 3 accept queue:
    client A
    client B
    client C
    ...
    client J
```

The listening socket is readable, so `epoll_wait()` returns.

Typically it may return one ready event:

```cpp
event_count = 1;

events[0].data.fd = 3;
events[0].events = EPOLLIN;
```

The event loop dispatches it:

```cpp
callback(3, EPOLLIN);
```

Then `handle_event()` sees:

```cpp
if (fd == server_fd)
```

and calls:

```cpp
accept_clients(loop);
```

## Accepting all ten clients

`accept_clients()` runs a loop:

```cpp
while (true) {
    int client_fd = accept(server_fd, ...);
    ...
}
```

The sequence is approximately:

```text
accept() -> fd 5
set_nonblocking(5)
epoll_ctl(4, EPOLL_CTL_ADD, 5, EPOLLIN)
connections.add(5)

accept() -> fd 6
set_nonblocking(6)
epoll_ctl(4, EPOLL_CTL_ADD, 6, EPOLLIN)
connections.add(6)

...
accept() -> fd 14
set_nonblocking(14)
epoll_ctl(4, EPOLL_CTL_ADD, 14, EPOLLIN)
connections.add(14)
```

The epoll registration set becomes:

```text
epoll instance 4:
    fd 3  -> EPOLLIN   listening socket
    fd 5  -> EPOLLIN   client A
    fd 6  -> EPOLLIN   client B
    fd 7  -> EPOLLIN   client C
    ...
    fd 14 -> EPOLLIN   client J
```

After the tenth client, the loop calls `accept()` again:

```cpp
accept() == -1
errno == EAGAIN || errno == EWOULDBLOCK
```

That means:

> The accept queue is currently empty.

So `accept_clients()` returns.

## Does the server respond to all clients in parallel?

No. This code is single-threaded, so it responds sequentially.

However, it uses nonblocking I/O and `epoll`, so it can efficiently alternate between clients instead of waiting for one client.

The next loop iteration calls:

```cpp
epoll_wait(4, events, 64, -1);
```

Suppose clients A, C, and J have sent data. It might return:

```cpp
event_count = 3;

events[0].data.fd = 5;
events[0].events = EPOLLIN;

events[1].data.fd = 7;
events[1].events = EPOLLIN;

events[2].data.fd = 14;
events[2].events = EPOLLIN;
```

Then this loop processes them one by one:

```cpp
for (int index = 0; index < event_count; ++index) {
    callback(events[index].data.fd, events[index].events);
}
```

### Client A

```cpp
callback(5, EPOLLIN);
```

`handle_event()` calls:

```cpp
handle_client(loop, 5);
```

`handle_client()`:

1. Reads available data from client A.
2. Sends the data back to client A.
3. Reads again until `read()` returns `EAGAIN`.
4. Returns.

### Client C

Then the loop processes client C:

```cpp
callback(7, EPOLLIN);
```

The same read-and-echo sequence runs for client C.

### Client J

Then it processes client J:

```cpp
callback(14, EPOLLIN);
```

Again, it reads and echoes sequentially.

Only after all returned events are processed does the server call `epoll_wait()` again.

## Full operation sequence

The overall sequence looks like this:

```text
epoll_create1()
    |
epoll_ctl(ADD, server_fd=3, EPOLLIN)
    |
epoll_wait()
    |
    | ten clients connect
    v
returns: server_fd=3 is ready
    |
accept(fd 5)
epoll_ctl(ADD, fd 5, EPOLLIN)
    |
accept(fd 6)
epoll_ctl(ADD, fd 6, EPOLLIN)
    |
...
    |
accept(fd 14)
epoll_ctl(ADD, fd 14, EPOLLIN)
    |
accept() -> EAGAIN
    |
epoll_wait()
    |
    | clients 5, 7, and 14 have data
    v
returns events for fd 5, fd 7, fd 14
    |
read/send fd 5
    |
read/send fd 7
    |
read/send fd 14
    |
epoll_wait()
```

## What “simultaneous” means here

The clients may connect or send data at nearly the same time, but this server executes one C++ instruction at a time on one thread:

```text
accept client A
accept client B
accept client C
...
read client A
send to client A
read client B
send to client B
...
```

It is concurrent in the sense that the server monitors all clients without blocking on idle ones. It is not parallel in the sense of multiple clients being processed by different CPU threads.

One important limitation in this implementation is that `send()` is performed immediately inside `handle_client()`. If a client's outgoing buffer becomes full, the nonblocking `send()` can return `EAGAIN`, but this code treats any `result <= 0` as a failure and closes that client. For small echo messages this is usually fine, but high-volume clients would need separate `EPOLLOUT` handling and an output buffer.