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

## Logs

Application logs are emitted as single-line key/value records:

```text
time=2026-07-09T11:52:57+0100 level=INFO event=server_started host=0.0.0.0 port=5555 max_clients=64 backlog=16 buffer_size=4096 websocket=true raw_tcp=true
```

Common events include `server_started`, `client_connected`,
`client_protocol_selected`, `message_broadcast`, `client_disconnected`, and
`websocket_frame_rejected`.

## Test

```sh
make test
```

## Docker

```sh
docker build -t socket-server:latest .
docker run --rm -p 5555:5555 socket-server:latest
```

The GitHub Actions release workflow:

1. Runs `make clean && make test && make`.
2. Bumps `VERSION`.
3. Updates `infra/manifests/deployment.yaml` to the new image tag.
4. Commits the version bump and creates an annotated `vX.Y.Z` tag.
5. Builds and publishes the Docker image.
6. Creates a GitHub Release for the tag.

Published image tags:

```text
ghcr.io/<owner>/<repo>:<version>
ghcr.io/<owner>/<repo>:latest
```

Automatic runs bump the minor version by default. Manual runs can choose
`major`, `minor`, or `patch`.

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
