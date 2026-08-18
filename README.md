# Bonfire

A terminal chat app built in C featuring public channels, private messages, and quality-of-life extras (colored names, emoji, ASCII stickers), all over a custom packed binary protocol on TCP.

## Notable Features

- **Public channels**: create and join channels with persistent message history.
- **Direct messages**: `/friend` opens a private channel with another user, invite list persisted across restarts.
- **Colored usernames**: personalize your display name with one of 7 colors.
- **Emoji shortcuts**: 12 shortcuts parsed into unicode live in chat.
- **ASCII art stickers**: 10 classic meme stickers sent inline in chat.
- **Live online-user counts**: real-time member counts for every channel.
- **1024 concurrent connections**: single-threaded `select()` server multiplexing up to 1024 clients.

## Build & Run

Requires gcc and a POSIX system.

    make

Produces two binaries, `server` and `client`, using the default port 51984.
To change the port, edit the `PORT` variable in the Makefile.

Start the server in one terminal:

    ./server

Start a client in another:

    ./client

The client connects to its own hostname (i.e. the same machine), so run both
locally or adjust the address resolution in `client.c` to point elsewhere.

Note: the `channels/` directory holds per-channel message history as binary
`.dat` files and must be present (it ships with the repo).

## Chat Commands

    /join <channel>    join a channel
    /create <channel>  create and join a channel
    /leave             leave the current channel
    /channels          list channels and how many users are online
    /friend <user>     send a friend request / open a private DM channel
    /color <color>     change your username color
    /colorlist         list available colors
    /emoji             list emoji shorthand
    /ascii             list ASCII art stickers
    /help              list commands
    /exit              quit

## Application Architecture

The wire protocol is a single packed 328-byte struct with no padding, sent as
raw bytes:

| Offset | Field | Size | Description |
|-------:|-------|-----:|-------------|
| 0 | action | 8 B | message type, e.g. `LOGIN`, `SEND`, `SYS` |
| 8 | data_size | 4 B | payload length |
| 12 | user | 16 B | sender username |
| 28 | username_colour | 4 B | ANSI color code |
| 32 | channel | 32 B | target channel |
| 64 | data | 256 B | message payload |
| 320 | timestamp | 8 B | unix time (seconds) |

There are 16 protocol actions covering login, channel create/join/leave, friend
requests, message send, system messages, and history.

The server is a single-threaded event loop built on `select()`. Each client
gets 16KB input and output buffers; messages are appended to a per-channel
`.dat` file and broadcast to channel members using a 1024-bit membership
bitmap. The client multiplexes stdin and the socket with its own `select()`
loop and runs the terminal in raw mode so it can redraw the prompt when
messages arrive mid-typing.
