#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

static volatile sig_atomic_t keep_running = 1;

struct client {
  int fd;
  char label[64];
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

static void disconnect_client(struct client *clients, int index) {
  if (clients[index].fd >= 0) {
    printf("client disconnected: %s\n", clients[index].label);
    close(clients[index].fd);
    clients[index].fd = -1;
    clients[index].label[0] = '\0';
  }
}

static void broadcast_to_peers(struct client *clients, int max_clients, int sender_index,
                               const unsigned char *data, size_t len) {
  for (int i = 0; i < max_clients; i++) {
    if (i == sender_index || clients[i].fd < 0) {
      continue;
    }

    if (!write_all(clients[i].fd, data, len)) {
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
  label_client(client_fd, clients[slot].label, sizeof(clients[slot].label));
  printf("client connected: %s\n", clients[slot].label);
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

      broadcast_to_peers(clients, max_clients, i, buffer, (size_t)nread);
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
