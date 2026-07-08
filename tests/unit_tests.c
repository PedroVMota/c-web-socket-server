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
  clients[1].fd = peer[0];
  strcpy(clients[1].label, "peer");

  broadcast_to_peers(clients, 2, 0, message, sizeof(message));
  assert(recv(peer[1], received, sizeof(received), 0) == (ssize_t)sizeof(message));
  assert(memcmp(received, message, sizeof(message)) == 0);

  close(sender[0]);
  close(sender[1]);
  close(peer[0]);
  close(peer[1]);
}

int main(void) {
  test_env_or_default();
  test_env_int_or_default();
  test_write_all();
  test_broadcast_to_peers();

  puts("unit tests passed");
  return 0;
}
