# Docs Backend

Docs Backend is a multi-client collaborative Markdown editing server written in C. It was built to explore POSIX processes, real-time signals, named pipes, threads, mutexes, role-based permissions, document versioning, and synchronized command processing.

The server keeps one shared Markdown document in memory. Clients connect using the server PID and a username, receive the current document and version, and submit edit commands. A background server thread orders queued commands by timestamp, applies authorized changes, increments the document version, and broadcasts the results to every connected client.

## Key features

- Concurrent client sessions managed with POSIX threads
- Bidirectional client/server communication through per-client FIFOs
- Real-time signal handshake for new connections
- Read and write roles loaded from `roles.txt`
- Mutex-protected client, command, document, and log state
- Versioned Markdown edits with cursor and stale-version validation
- Insert, delete, heading, bold, italic, blockquote, list, code, rule, and link operations
- Server and client document/log inspection commands
- AddressSanitizer-enabled development build

## Architecture

```text
client process ── SIGRTMIN + PID ──> server
client process <──── named FIFOs ───> client thread
                                      │
                                      ▼
                               command queue
                                      │
                                      ▼
                             synchronized editor
                                      │
                                      ▼
                           broadcast to all clients
```

`source/server.c` manages connections, authorization, command ordering, synchronization, and broadcasts. `source/client.c` maintains a local document view and sends commands. `source/markdown.c` implements the versioned Markdown editing model declared in `libs/markdown.h`.

## Requirements

- Linux or another POSIX environment supporting `SIGRTMIN`
- GCC or a compatible C compiler
- POSIX threads and named pipes
- `make`

> macOS does not provide POSIX real-time signals such as `SIGRTMIN`, so the current handshake is Linux-specific.

## Build and run

```bash
make
./server 1000
```

The server prints its PID. In another terminal, connect a user listed in `roles.txt`:

```bash
./client <SERVER_PID> yuwon
```

The server argument is the command-processing interval in milliseconds. Add or update users in `roles.txt` using the format `username read` or `username write` before connecting.

## Client commands

Read-only inspection commands are available to every authorized client:

```text
DOC?
PERM?
LOG?
DISCONNECT
```

Users with the `write` role can submit editing commands such as:

```text
INSERT <position> <content>
DELETE <position> <length>
NEWLINE <position>
HEADING <level> <position>
BOLD <start> <end>
ITALIC <start> <end>
BLOCKQUOTE <position>
ORDERED_LIST <position>
UNORDERED_LIST <position>
CODE <start> <end>
HORIZONTAL_RULE <position>
LINK <start> <end> <url>
```

The server console also accepts `DOC?`, `LOG?`, and `QUIT`. `QUIT` is rejected while clients remain connected.

## Clean build artifacts

```bash
make clean
```

This removes compiled binaries, object files, and generated FIFO files.
