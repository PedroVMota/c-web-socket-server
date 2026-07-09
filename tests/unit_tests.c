#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define main chat_server_main
#include "src/chat_server.c"
#undef main

static void test_env_or_default(void) {
  unsetenv("CHAT_TEST_STRING");
  assert(strcmp(env_or_default("CHAT_TEST_STRING", "fallback"), "fallback") == 0);

  setenv("CHAT_TEST_STRING", "", 1);
  assert(strcmp(env_or_default("CHAT_TEST_STRING", "fallback"), "fallback") == 0);

  setenv("CHAT_TEST_STRING", "configured", 1);
  assert(strcmp(env_or_default("CHAT_TEST_STRING", "fallback"), "configured") == 0);

  unsetenv("CHAT_TEST_STRING");
}

static void test_env_int_or_default(void) {
  unsetenv("CHAT_TEST_INT");
  assert(env_int_or_default("CHAT_TEST_INT", 7, 1, 10) == 7);

  setenv("CHAT_TEST_INT", "9", 1);
  assert(env_int_or_default("CHAT_TEST_INT", 7, 1, 10) == 9);

  setenv("CHAT_TEST_INT", "0", 1);
  assert(env_int_or_default("CHAT_TEST_INT", 7, 1, 10) == 7);

  setenv("CHAT_TEST_INT", "11", 1);
  assert(env_int_or_default("CHAT_TEST_INT", 7, 1, 10) == 7);

  setenv("CHAT_TEST_INT", "not-a-number", 1);
  assert(env_int_or_default("CHAT_TEST_INT", 7, 1, 10) == 7);

  unsetenv("CHAT_TEST_INT");
}

static void test_write_all(void) {
  int pair[2];
  const unsigned char message[] = "hello socket test";
  unsigned char received[sizeof(message)];

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(write_all(pair[0], message, sizeof(message)) == true);
  assert(recv(pair[1], received, sizeof(received), 0) == (ssize_t)sizeof(message));
  assert(memcmp(received, message, sizeof(message)) == 0);

  close(pair[0]);
  close(pair[1]);
}

static void test_websocket_accept_key(void) {
  char accept_key[64];

  assert(websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept_key, sizeof(accept_key)) == true);
  assert(strcmp(accept_key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
}

static void test_websocket_handshake(void) {
  int pair[2];
  const unsigned char request[] =
      "GET /chat HTTP/1.1\r\n"
      "Host: localhost:5555\r\n"
      "Upgrade: websocket\r\n"
      "Connection: keep-alive, Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";
  char response[512];
  ssize_t nread;

  assert(is_websocket_handshake(request, sizeof(request) - 1) == true);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(send_websocket_handshake(pair[0], request, sizeof(request) - 1) == true);

  nread = recv(pair[1], response, sizeof(response) - 1, 0);
  assert(nread > 0);
  response[nread] = '\0';
  assert(strstr(response, "HTTP/1.1 101 Switching Protocols\r\n") != NULL);
  assert(strstr(response, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != NULL);

  close(pair[0]);
  close(pair[1]);
}

static void test_write_websocket_frame(void) {
  int pair[2];
  const unsigned char message[] = "hi";
  unsigned char received[4];

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(write_websocket_frame(pair[0], 0x1, message, sizeof(message) - 1) == true);
  assert(recv(pair[1], received, sizeof(received), 0) == (ssize_t)sizeof(received));
  assert(received[0] == 0x81);
  assert(received[1] == 0x02);
  assert(received[2] == 'h');
  assert(received[3] == 'i');

  close(pair[0]);
  close(pair[1]);
}

static void test_broadcast_to_peers(void) {
  int sender[2];
  int peer[2];
  struct client clients[2];
  const unsigned char message[] = {'o', 'k', '\0', '!', '\n'};
  unsigned char received[sizeof(message)];

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sender) == 0);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, peer) == 0);

  clients[0].fd = sender[0];
  strcpy(clients[0].label, "sender");
  clients[0].mode = CLIENT_RAW;
  clients[1].fd = peer[0];
  strcpy(clients[1].label, "peer");
  clients[1].mode = CLIENT_RAW;

  broadcast_to_peers(clients, 2, 0, message, sizeof(message), 0x2);
  assert(recv(peer[1], received, sizeof(received), 0) == (ssize_t)sizeof(message));
  assert(memcmp(received, message, sizeof(message)) == 0);

  close(sender[0]);
  close(sender[1]);
  close(peer[0]);
  close(peer[1]);
}

static void test_websocket_text_broadcast(void) {
  int sender[2];
  int peer[2];
  struct client clients[2];
  const unsigned char frame[] = {0x81, 0x82, 0x01, 0x02, 0x03, 0x04,
                                 'o' ^ 0x01, 'k' ^ 0x02};
  unsigned char received[4];

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sender) == 0);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, peer) == 0);

  clients[0].fd = sender[0];
  strcpy(clients[0].label, "sender");
  clients[0].mode = CLIENT_WEBSOCKET;
  clients[1].fd = peer[0];
  strcpy(clients[1].label, "peer");
  clients[1].mode = CLIENT_WEBSOCKET;

  assert(handle_client_data(clients, 2, 0, frame, sizeof(frame)) == true);
  assert(recv(peer[1], received, sizeof(received), 0) == (ssize_t)sizeof(received));
  assert(received[0] == 0x81);
  assert(received[1] == 0x02);
  assert(received[2] == 'o');
  assert(received[3] == 'k');

  close(sender[0]);
  close(sender[1]);
  close(peer[0]);
  close(peer[1]);
}

static void test_websocket_ping_pong(void) {
  int pair[2];
  struct client clients[1];
  const unsigned char frame[] = {0x89, 0x82, 0x01, 0x02, 0x03, 0x04,
                                 'p' ^ 0x01, 'i' ^ 0x02};
  unsigned char received[4];

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  clients[0].fd = pair[0];
  strcpy(clients[0].label, "client");
  clients[0].mode = CLIENT_WEBSOCKET;

  assert(handle_client_data(clients, 1, 0, frame, sizeof(frame)) == true);
  assert(recv(pair[1], received, sizeof(received), 0) == (ssize_t)sizeof(received));
  assert(received[0] == 0x8A);
  assert(received[1] == 0x02);
  assert(received[2] == 'p');
  assert(received[3] == 'i');

  close(pair[0]);
  close(pair[1]);
}

int main(void) {
  test_env_or_default();
  test_env_int_or_default();
  test_write_all();
  test_websocket_accept_key();
  test_websocket_handshake();
  test_write_websocket_frame();
  test_broadcast_to_peers();
  test_websocket_text_broadcast();
  test_websocket_ping_pong();

  puts("unit tests passed");
  return 0;
}
