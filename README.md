# C Chat Server

Small TCP chat server written in C. It accepts arbitrary raw TCP payloads and
`ws://` WebSocket clients on the same port, then broadcasts received messages to
all other connected clients.

## Build

```sh
make
```

## Run

```sh
CHAT_HOST=0.0.0.0 CHAT_PORT=5555 ./chat-server
```

Raw TCP clients keep the original behavior. WebSocket clients can connect with:

```js
const ws = new WebSocket("ws://localhost:5555");
ws.onmessage = (event) => console.log(event.data);
ws.onopen = () => ws.send("hello from websocket");
```

The server implements the RFC 6455 HTTP upgrade handshake, masked client frames,
text/binary broadcast frames, ping/pong, and close frames. TLS is not included,
so use `ws://` rather than `wss://`.

## Test

```sh
make test
```

## Docker

```sh
docker build -t socket-server:latest .
docker run --rm -p 5555:5555 socket-server:latest
```

The GitHub Actions workflow publishes:

```text
ghcr.io/<owner>/<repo>:latest
```

## Environment

| Variable | Default | Description |
| --- | --- | --- |
| `CHAT_HOST` | `0.0.0.0` | Address to bind. |
| `CHAT_PORT` | `5555` | TCP port to listen on. |
| `CHAT_MAX_CLIENTS` | `64` | Maximum simultaneous clients. |
| `CHAT_BACKLOG` | `16` | Listen backlog. |
| `CHAT_BUFFER_SIZE` | `4096` | Bytes read per socket receive. |

## Test Client

The companion test client lives at:

```text
../socket-client
```
