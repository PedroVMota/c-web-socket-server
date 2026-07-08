# C Chat Server

Small TCP chat server written in C. It accepts arbitrary client payloads and
broadcasts every received byte sequence to all other connected clients.

## Build

```sh
make
```

## Run

```sh
CHAT_HOST=0.0.0.0 CHAT_PORT=5555 ./chat-server
```

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
