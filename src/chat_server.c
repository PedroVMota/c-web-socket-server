#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_HOST "0.0.0.0"
#define DEFAULT_PORT "5555"
#define DEFAULT_MAX_CLIENTS 64
#define DEFAULT_BACKLOG 16
#define DEFAULT_BUFFER_SIZE 4096
#define WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

enum client_mode {
  CLIENT_UNKNOWN = 0,
  CLIENT_RAW,
  CLIENT_WEBSOCKET
};

static volatile sig_atomic_t keep_running = 1;

struct client {
  int fd;
  char label[64];
  enum client_mode mode;
};

static void handle_signal(int signo) {
  (void)signo;
  keep_running = 0;
}

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value && value[0] != '\0' ? value : fallback;
}

static int env_int_or_default(const char *name, int fallback, int min, int max) {
  const char *raw = getenv(name);
  char *end = NULL;
  long value;

  if (!raw || raw[0] == '\0') {
    return fallback;
  }

  errno = 0;
  value = strtol(raw, &end, 10);
  if (errno != 0 || !end || *end != '\0' || value < min || value > max) {
    fprintf(stderr, "Invalid %s=%s, using %d\n", name, raw, fallback);
    return fallback;
  }

  return (int)value;
}

static int set_reuseaddr(int fd) {
  int enabled = 1;
  return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
}

static int create_listener(const char *host, const char *port, int backlog) {
  struct addrinfo hints;
  struct addrinfo *result = NULL;
  struct addrinfo *rp = NULL;
  int listener = -1;
  int rc;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  rc = getaddrinfo(host, port, &hints, &result);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
    return -1;
  }

  for (rp = result; rp; rp = rp->ai_next) {
    listener = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (listener < 0) {
      continue;
    }

    if (set_reuseaddr(listener) < 0) {
      perror("setsockopt SO_REUSEADDR");
      close(listener);
      listener = -1;
      continue;
    }

    if (bind(listener, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }

    perror("bind");
    close(listener);
    listener = -1;
  }

  freeaddrinfo(result);

  if (listener < 0) {
    return -1;
  }

  if (listen(listener, backlog) < 0) {
    perror("listen");
    close(listener);
    return -1;
  }

  return listener;
}

static bool write_all(int fd, const unsigned char *data, size_t len) {
  size_t sent = 0;

  while (sent < len) {
    ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    sent += (size_t)n;
  }

  return true;
}

struct sha1_ctx {
  uint32_t state[5];
  uint64_t bit_count;
  unsigned char buffer[64];
  size_t buffer_len;
};

static uint32_t rol32(uint32_t value, unsigned int bits) {
  return (value << bits) | (value >> (32U - bits));
}

static void sha1_transform(uint32_t state[5], const unsigned char block[64]) {
  uint32_t w[80];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;

  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (int i = 16; i < 80; i++) {
    w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];

  for (int i = 0; i < 80; i++) {
    uint32_t f;
    uint32_t k;
    uint32_t temp;

    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999U;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1U;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCU;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6U;
    }

    temp = rol32(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rol32(b, 30);
    b = a;
    a = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

static void sha1_init(struct sha1_ctx *ctx) {
  ctx->state[0] = 0x67452301U;
  ctx->state[1] = 0xEFCDAB89U;
  ctx->state[2] = 0x98BADCFEU;
  ctx->state[3] = 0x10325476U;
  ctx->state[4] = 0xC3D2E1F0U;
  ctx->bit_count = 0;
  ctx->buffer_len = 0;
}

static void sha1_update(struct sha1_ctx *ctx, const unsigned char *data, size_t len) {
  ctx->bit_count += (uint64_t)len * 8U;

  while (len > 0) {
    size_t copy = sizeof(ctx->buffer) - ctx->buffer_len;
    if (copy > len) {
      copy = len;
    }

    memcpy(ctx->buffer + ctx->buffer_len, data, copy);
    ctx->buffer_len += copy;
    data += copy;
    len -= copy;

    if (ctx->buffer_len == sizeof(ctx->buffer)) {
      sha1_transform(ctx->state, ctx->buffer);
      ctx->buffer_len = 0;
    }
  }
}

static void sha1_final(struct sha1_ctx *ctx, unsigned char digest[20]) {
  uint64_t bits = ctx->bit_count;

  ctx->buffer[ctx->buffer_len++] = 0x80;
  if (ctx->buffer_len > 56) {
    while (ctx->buffer_len < sizeof(ctx->buffer)) {
      ctx->buffer[ctx->buffer_len++] = 0;
    }
    sha1_transform(ctx->state, ctx->buffer);
    ctx->buffer_len = 0;
  }

  while (ctx->buffer_len < 56) {
    ctx->buffer[ctx->buffer_len++] = 0;
  }

  for (int i = 7; i >= 0; i--) {
    ctx->buffer[ctx->buffer_len++] = (unsigned char)(bits >> (i * 8));
  }
  sha1_transform(ctx->state, ctx->buffer);

  for (int i = 0; i < 5; i++) {
    digest[i * 4] = (unsigned char)(ctx->state[i] >> 24);
    digest[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
    digest[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
    digest[i * 4 + 3] = (unsigned char)ctx->state[i];
  }
}

static size_t base64_encode(const unsigned char *input, size_t len, char *output, size_t output_len) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t out = 0;

  for (size_t i = 0; i < len; i += 3) {
    unsigned int value = (unsigned int)input[i] << 16;
    int remaining = (int)(len - i);

    if (remaining > 1) {
      value |= (unsigned int)input[i + 1] << 8;
    }
    if (remaining > 2) {
      value |= input[i + 2];
    }
    if (out + 4 >= output_len) {
      return 0;
    }

    output[out++] = table[(value >> 18) & 0x3FU];
    output[out++] = table[(value >> 12) & 0x3FU];
    output[out++] = remaining > 1 ? table[(value >> 6) & 0x3FU] : '=';
    output[out++] = remaining > 2 ? table[value & 0x3FU] : '=';
  }

  if (out >= output_len) {
    return 0;
  }
  output[out] = '\0';
  return out;
}

static bool header_contains_token(const char *request, const char *header, const char *token) {
  size_t header_len = strlen(header);
  const char *line = request;

  while (*line != '\0') {
    const char *line_end = strstr(line, "\r\n");
    size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);

    if (line_len > header_len && strncasecmp(line, header, header_len) == 0 &&
        line[header_len] == ':') {
      const char *value = line + header_len + 1;
      const char *end = line + line_len;

      while (value < end) {
        while (value < end && (*value == ' ' || *value == '\t' || *value == ',')) {
          value++;
        }

        const char *token_end = value;
        while (token_end < end && *token_end != ',') {
          token_end++;
        }

        while (token_end > value && (token_end[-1] == ' ' || token_end[-1] == '\t')) {
          token_end--;
        }

        if ((size_t)(token_end - value) == strlen(token) &&
            strncasecmp(value, token, strlen(token)) == 0) {
          return true;
        }
        value = token_end + 1;
      }
    }

    if (!line_end) {
      break;
    }
    line = line_end + 2;
  }

  return false;
}

static bool get_header_value(const char *request, const char *header, char *value, size_t value_len) {
  size_t header_len = strlen(header);
  const char *line = request;

  while (*line != '\0') {
    const char *line_end = strstr(line, "\r\n");
    size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);

    if (line_len > header_len && strncasecmp(line, header, header_len) == 0 &&
        line[header_len] == ':') {
      const char *start = line + header_len + 1;
      const char *end = line + line_len;
      size_t copy_len;

      while (start < end && (*start == ' ' || *start == '\t')) {
        start++;
      }
      while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
      }

      copy_len = (size_t)(end - start);
      if (copy_len >= value_len) {
        return false;
      }

      memcpy(value, start, copy_len);
      value[copy_len] = '\0';
      return true;
    }

    if (!line_end) {
      break;
    }
    line = line_end + 2;
  }

  return false;
}

static bool websocket_accept_key(const char *client_key, char *accept_key, size_t accept_key_len) {
  struct sha1_ctx ctx;
  unsigned char digest[20];
  char combined[128];

  if (snprintf(combined, sizeof(combined), "%s%s", client_key, WEBSOCKET_GUID) >=
      (int)sizeof(combined)) {
    return false;
  }

  sha1_init(&ctx);
  sha1_update(&ctx, (const unsigned char *)combined, strlen(combined));
  sha1_final(&ctx, digest);

  return base64_encode(digest, sizeof(digest), accept_key, accept_key_len) > 0;
}

static bool is_websocket_handshake(const unsigned char *data, size_t len) {
  char request[4096];

  if (len >= sizeof(request) || len < 14) {
    return false;
  }

  memcpy(request, data, len);
  request[len] = '\0';

  return strncmp(request, "GET ", 4) == 0 && strstr(request, "\r\n\r\n") != NULL &&
         header_contains_token(request, "Connection", "Upgrade") &&
         header_contains_token(request, "Upgrade", "websocket");
}

static bool send_websocket_handshake(int fd, const unsigned char *data, size_t len) {
  char request[4096];
  char client_key[128];
  char accept_key[64];
  char response[256];
  int response_len;

  if (len >= sizeof(request)) {
    return false;
  }
  memcpy(request, data, len);
  request[len] = '\0';

  if (!get_header_value(request, "Sec-WebSocket-Key", client_key, sizeof(client_key)) ||
      !websocket_accept_key(client_key, accept_key, sizeof(accept_key))) {
    return false;
  }

  response_len = snprintf(response, sizeof(response),
                          "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n"
                          "\r\n",
                          accept_key);
  if (response_len < 0 || response_len >= (int)sizeof(response)) {
    return false;
  }

  return write_all(fd, (const unsigned char *)response, (size_t)response_len);
}

static bool write_websocket_frame(int fd, unsigned char opcode, const unsigned char *data, size_t len) {
  unsigned char header[10];
  size_t header_len = 0;

  header[header_len++] = (unsigned char)(0x80U | (opcode & 0x0FU));
  if (len <= 125) {
    header[header_len++] = (unsigned char)len;
  } else if (len <= 65535) {
    header[header_len++] = 126;
    header[header_len++] = (unsigned char)(len >> 8);
    header[header_len++] = (unsigned char)len;
  } else {
    header[header_len++] = 127;
    for (int i = 7; i >= 0; i--) {
      header[header_len++] = (unsigned char)((uint64_t)len >> (i * 8));
    }
  }

  return write_all(fd, header, header_len) && write_all(fd, data, len);
}

static bool write_client_payload(struct client *client, const unsigned char *data, size_t len,
                                 unsigned char websocket_opcode) {
  if (client->mode == CLIENT_WEBSOCKET) {
    return write_websocket_frame(client->fd, websocket_opcode, data, len);
  }

  return write_all(client->fd, data, len);
}

static void disconnect_client(struct client *clients, int index) {
  if (clients[index].fd >= 0) {
    printf("client disconnected: %s\n", clients[index].label);
    close(clients[index].fd);
    clients[index].fd = -1;
    clients[index].label[0] = '\0';
    clients[index].mode = CLIENT_UNKNOWN;
  }
}

static void broadcast_to_peers(struct client *clients, int max_clients, int sender_index,
                               const unsigned char *data, size_t len, unsigned char websocket_opcode) {
  for (int i = 0; i < max_clients; i++) {
    if (i == sender_index || clients[i].fd < 0) {
      continue;
    }

    if (!write_client_payload(&clients[i], data, len, websocket_opcode)) {
      disconnect_client(clients, i);
    }
  }
}

static void label_client(int fd, char *label, size_t label_len) {
  struct sockaddr_storage addr;
  socklen_t addr_len = sizeof(addr);
  char host[1025];
  char service[32];

  if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) == 0 &&
      getnameinfo((struct sockaddr *)&addr, addr_len, host, sizeof(host), service,
                  sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
    snprintf(label, label_len, "%.48s:%.12s", host, service);
    return;
  }

  snprintf(label, label_len, "fd:%d", fd);
}

static void accept_client(int listener, struct client *clients, int max_clients) {
  int client_fd = accept(listener, NULL, NULL);
  int slot = -1;

  if (client_fd < 0) {
    if (errno != EINTR) {
      perror("accept");
    }
    return;
  }

  for (int i = 0; i < max_clients; i++) {
    if (clients[i].fd < 0) {
      slot = i;
      break;
    }
  }

  if (slot < 0) {
    static const char full[] = "server full\n";
    (void)write_all(client_fd, (const unsigned char *)full, sizeof(full) - 1);
    close(client_fd);
    return;
  }

  clients[slot].fd = client_fd;
  clients[slot].mode = CLIENT_UNKNOWN;
  label_client(client_fd, clients[slot].label, sizeof(clients[slot].label));
  printf("client connected: %s\n", clients[slot].label);
}

static bool handle_websocket_frame(struct client *clients, int max_clients, int sender_index,
                                   const unsigned char *data, size_t len) {
  size_t pos = 0;

  while (pos < len) {
    unsigned char opcode;
    bool masked;
    uint64_t payload_len;
    unsigned char mask[4];
    unsigned char payload[65536];
    size_t header_len = 2;

    if (len - pos < 2) {
      return false;
    }

    opcode = data[pos] & 0x0FU;
    masked = (data[pos + 1] & 0x80U) != 0;
    payload_len = data[pos + 1] & 0x7FU;

    if ((data[pos] & 0x70U) != 0 || !masked) {
      return false;
    }

    if (payload_len == 126) {
      if (len - pos < 4) {
        return false;
      }
      payload_len = ((uint64_t)data[pos + 2] << 8) | data[pos + 3];
      header_len = 4;
    } else if (payload_len == 127) {
      if (len - pos < 10) {
        return false;
      }
      payload_len = 0;
      for (int i = 0; i < 8; i++) {
        payload_len = (payload_len << 8) | data[pos + 2 + i];
      }
      header_len = 10;
    }

    if (payload_len > sizeof(payload) || len - pos < header_len + 4 + payload_len) {
      return false;
    }

    memcpy(mask, data + pos + header_len, sizeof(mask));
    for (uint64_t i = 0; i < payload_len; i++) {
      payload[i] = data[pos + header_len + 4 + i] ^ mask[i % 4];
    }

    if (opcode == 0x1 || opcode == 0x2) {
      broadcast_to_peers(clients, max_clients, sender_index, payload, (size_t)payload_len, opcode);
    } else if (opcode == 0x8) {
      (void)write_websocket_frame(clients[sender_index].fd, 0x8, payload, (size_t)payload_len);
      return false;
    } else if (opcode == 0x9) {
      if (!write_websocket_frame(clients[sender_index].fd, 0xA, payload, (size_t)payload_len)) {
        return false;
      }
    } else if (opcode != 0xA) {
      return false;
    }

    pos += header_len + 4 + (size_t)payload_len;
  }

  return true;
}

static bool handle_client_data(struct client *clients, int max_clients, int index,
                               const unsigned char *data, size_t len) {
  if (clients[index].mode == CLIENT_UNKNOWN) {
    if (is_websocket_handshake(data, len)) {
      if (!send_websocket_handshake(clients[index].fd, data, len)) {
        return false;
      }
      clients[index].mode = CLIENT_WEBSOCKET;
      return true;
    }

    clients[index].mode = CLIENT_RAW;
  }

  if (clients[index].mode == CLIENT_WEBSOCKET) {
    return handle_websocket_frame(clients, max_clients, index, data, len);
  }

  broadcast_to_peers(clients, max_clients, index, data, len, 0x2);
  return true;
}

static int serve(int listener, int max_clients, int buffer_size) {
  struct client *clients = calloc((size_t)max_clients, sizeof(*clients));
  unsigned char *buffer = malloc((size_t)buffer_size);

  if (!clients || !buffer) {
    perror("alloc");
    free(clients);
    free(buffer);
    return 1;
  }

  for (int i = 0; i < max_clients; i++) {
    clients[i].fd = -1;
    clients[i].mode = CLIENT_UNKNOWN;
  }

  while (keep_running) {
    fd_set readfds;
    int max_fd = listener;
    int ready;

    FD_ZERO(&readfds);
    FD_SET(listener, &readfds);

    for (int i = 0; i < max_clients; i++) {
      if (clients[i].fd >= 0) {
        FD_SET(clients[i].fd, &readfds);
        if (clients[i].fd > max_fd) {
          max_fd = clients[i].fd;
        }
      }
    }

    ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("select");
      break;
    }

    if (FD_ISSET(listener, &readfds)) {
      accept_client(listener, clients, max_clients);
    }

    for (int i = 0; i < max_clients; i++) {
      ssize_t nread;

      if (clients[i].fd < 0 || !FD_ISSET(clients[i].fd, &readfds)) {
        continue;
      }

      nread = recv(clients[i].fd, buffer, (size_t)buffer_size, 0);
      if (nread < 0) {
        if (errno != EINTR) {
          disconnect_client(clients, i);
        }
        continue;
      }
      if (nread == 0) {
        disconnect_client(clients, i);
        continue;
      }

      if (!handle_client_data(clients, max_clients, i, buffer, (size_t)nread)) {
        disconnect_client(clients, i);
      }
    }
  }

  for (int i = 0; i < max_clients; i++) {
    disconnect_client(clients, i);
  }

  free(clients);
  free(buffer);
  return 0;
}

int main(void) {
  const char *host = env_or_default("CHAT_HOST", DEFAULT_HOST);
  const char *port = env_or_default("CHAT_PORT", DEFAULT_PORT);
  int max_clients = env_int_or_default("CHAT_MAX_CLIENTS", DEFAULT_MAX_CLIENTS, 1, FD_SETSIZE - 1);
  int backlog = env_int_or_default("CHAT_BACKLOG", DEFAULT_BACKLOG, 1, 1024);
  int buffer_size = env_int_or_default("CHAT_BUFFER_SIZE", DEFAULT_BUFFER_SIZE, 1, 1048576);
  int listener;

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  listener = create_listener(host, port, backlog);
  if (listener < 0) {
    return 1;
  }

  printf("chat server listening on %s:%s\n", host, port);
  printf("max_clients=%d backlog=%d buffer_size=%d\n", max_clients, backlog, buffer_size);
  fflush(stdout);

  int rc = serve(listener, max_clients, buffer_size);
  close(listener);
  return rc;
}
