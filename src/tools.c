#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef ESP_PLATFORM
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #include "esp_timer.h"
#else
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/socket.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
  #ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC 1
  #endif
#endif

#include "globals.h"
#include "varnum.h"
#include "procedures.h"
#include "tools.h"
#include "websocket.h"

#ifndef htonll
  static uint64_t htonll (uint64_t value) {
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value >> 32))) |
           ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32);
  #else
    return value;
  #endif
  }
#endif

// Keep track of the total amount of bytes received with recv_all
// Helps notice misread packets and clean up after errors
uint64_t total_bytes_received = 0;

static ssize_t recv_raw_all (int client_fd, void *buf, size_t n, uint8_t require_first) {
  char *p = buf;
  size_t total = 0;

  // If requested, exit early when first byte not immediately available
  if (require_first) {
    ssize_t r = recv(client_fd, p, 1, MSG_PEEK);
    if (r <= 0) {
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0; // no first byte available yet
      }
      return -1; // error or connection closed
    }
  }

  // Busy-wait (with task yielding) until we get exactly n bytes
  while (total < n) {
    ssize_t r = recv(client_fd, p + total, n - total, 0);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        task_yield();
        continue;
      } else {
        total_bytes_received += total;
        return -1; // real error
      }
    } else if (r == 0) {
      total_bytes_received += total;
      return -1;
    }
    total += r;
  }

  total_bytes_received += total;
  return total; // got exactly n bytes
}

static ssize_t send_raw_all (int client_fd, const void *buf, ssize_t len) {
  // Treat any input buffer as *uint8_t for simplicity
  const uint8_t *p = (const uint8_t *)buf;
  ssize_t sent = 0;

  // Track time of last meaningful network update
  // Used to handle timeout when client is stalling
  int64_t last_update_time = get_program_time();

  // Busy-wait (with task yielding) until all data has been sent
  while (sent < len) {
    #ifdef _WIN32
      ssize_t n = send(client_fd, p + sent, len - sent, 0);
    #else
      ssize_t n = send(client_fd, p + sent, len - sent, MSG_NOSIGNAL);
    #endif
    if (n > 0) { // some data was sent, log it
      sent += n;
      last_update_time = get_program_time();
      continue;
    }
    if (n == 0) { // connection was closed, treat this as an error
      errno = ECONNRESET;
      return -1;
    }
    // not yet ready to transmit, try again
    #ifdef _WIN32 //handles windows socket timeout
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
    #else
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    #endif
      // handle network timeout
      if (get_program_time() - last_update_time > SEND_STALL_TIMEOUT_TIME) {
        disconnectClient(&client_fd, -2);
        return -1;
      }
      task_yield();
      continue;
    }
    return -1; // real error
  }

  return sent;
}

ssize_t recv_all (int client_fd, void *buf, size_t n, uint8_t require_first) {
  if (ws_is_open(client_fd)) return ws_recv(client_fd, buf, n, require_first);
  return recv_raw_all(client_fd, buf, n, require_first);
}

#define SEND_BUF_SIZE 8192
#define SEND_BUF_HEADER 10
static uint8_t send_buf[SEND_BUF_SIZE + SEND_BUF_HEADER];
static uint8_t *send_data = send_buf + SEND_BUF_HEADER;
static int send_buf_len = 0;
static int send_buf_fd = -1;

static void flush_send_buffer (void) {
  if (send_buf_fd < 0 || send_buf_len <= 0) {
    send_buf_len = 0;
    send_buf_fd = -1;
    return;
  }
  int fd = send_buf_fd;
  int len = send_buf_len;
  send_buf_len = 0;
  send_buf_fd = -1;
  if (!ws_is_open(fd)) send_raw_all(fd, send_data, len);
  else {
    uint8_t header[10];
    int header_len = ws_frame_header(header, (uint64_t) len);
    memcpy(send_data - header_len, header, header_len);
    send_raw_all(fd, send_data - header_len, header_len + len);
  }
}

void flush_send_all (void) {
  flush_send_buffer();
}

ssize_t send_all (int client_fd, const void *buf, ssize_t len) {
  if (len <= 0) return 0;
  if (send_buf_fd != client_fd) {
    flush_send_buffer();
    send_buf_fd = client_fd;
  }
  if (len >= SEND_BUF_SIZE) {
    flush_send_buffer();
    if (!ws_is_open(client_fd)) return send_raw_all(client_fd, buf, len);
    uint8_t header[10];
    int header_len = ws_frame_header(header, (uint64_t) len);
    if (send_raw_all(client_fd, header, header_len) < 0) return -1;
    return send_raw_all(client_fd, buf, len);
  }
  if (send_buf_len + len > SEND_BUF_SIZE) {
    flush_send_buffer();
    send_buf_fd = client_fd;
  }
  memcpy(send_data + send_buf_len, buf, len);
  send_buf_len += len;
  return len;
}

int conn_poll_readable (int client_fd) {
  if (ws_mode(client_fd) == WS_MODE_OPEN) return ws_poll(client_fd);

  uint8_t byte;
  ssize_t r = recv(client_fd, &byte, 1, MSG_PEEK);
  if (r > 0) return 1;
  #ifdef _WIN32
    if (r == 0) return -1;
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEINTR) return 0;
    return -1;
  #else
    if (r == 0) return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
  #endif
}

void discard_all (int client_fd, size_t remaining, uint8_t require_first) {
  while (remaining > 0) {
    size_t recv_n = remaining > MAX_RECV_BUF_LEN ? MAX_RECV_BUF_LEN : remaining;
    ssize_t received = recv_all(client_fd, recv_buffer, recv_n, require_first);
    if (received <= 0) return;
    if (received > (ssize_t)remaining) return;
    remaining -= received;
    require_first = false;
  }
}

ssize_t writeByte (int client_fd, uint8_t byte) {
  return send_all(client_fd, &byte, 1);
}
ssize_t writeUint16 (int client_fd, uint16_t num) {
  uint16_t be = htons(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint32 (int client_fd, uint32_t num) {
  uint32_t be = htonl(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint64 (int client_fd, uint64_t num) {
  uint64_t be = htonll(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeFloat (int client_fd, float num) {
  uint32_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonl(bits);
  return send_all(client_fd, &bits, sizeof(bits));
}
ssize_t writeDouble (int client_fd, double num) {
  uint64_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonll(bits);
  return send_all(client_fd, &bits, sizeof(bits));
}

uint8_t readByte (int client_fd) {
  uint8_t byte;
  recv_count = recv_all(client_fd, &byte, 1, false);
  if (recv_count != 1) return 0;
  return byte;
}
uint16_t readUint16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((uint16_t)recv_buffer[0] << 8) | recv_buffer[1];
}
int16_t readInt16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((int16_t)recv_buffer[0] << 8) | (int16_t)recv_buffer[1];
}
uint32_t readUint32 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 4, false);
  return ((uint32_t)recv_buffer[0] << 24) |
         ((uint32_t)recv_buffer[1] << 16) |
         ((uint32_t)recv_buffer[2] << 8) |
         ((uint32_t)recv_buffer[3]);
}
uint64_t readUint64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((uint64_t)recv_buffer[0] << 56) |
         ((uint64_t)recv_buffer[1] << 48) |
         ((uint64_t)recv_buffer[2] << 40) |
         ((uint64_t)recv_buffer[3] << 32) |
         ((uint64_t)recv_buffer[4] << 24) |
         ((uint64_t)recv_buffer[5] << 16) |
         ((uint64_t)recv_buffer[6] << 8) |
         ((uint64_t)recv_buffer[7]);
}
int64_t readInt64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((int64_t)recv_buffer[0] << 56) |
         ((int64_t)recv_buffer[1] << 48) |
         ((int64_t)recv_buffer[2] << 40) |
         ((int64_t)recv_buffer[3] << 32) |
         ((int64_t)recv_buffer[4] << 24) |
         ((int64_t)recv_buffer[5] << 16) |
         ((int64_t)recv_buffer[6] << 8) |
         ((int64_t)recv_buffer[7]);
}
float readFloat (int client_fd) {
  uint32_t bytes = readUint32(client_fd);
  float output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}
double readDouble (int client_fd) {
  uint64_t bytes = readUint64(client_fd);
  double output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}

// Receive length prefixed data with bounds checking
ssize_t readLengthPrefixedData (int client_fd) {
  uint32_t length = readVarInt(client_fd);
  if (length == (uint32_t)VARNUM_ERROR) {
    recv_count = -1;
    return -1;
  }
  if (length >= MAX_RECV_BUF_LEN) {
    if (length > MAX_RECV_BUF_LEN * 8) {
      printf("ERROR: Received length (%u) exceeds maximum (%u)\n",
             (unsigned)length, (unsigned)MAX_RECV_BUF_LEN);
      disconnectClient(&client_fd, -1);
      recv_count = 0;
      return 0;
    }
    recv_count = recv_all(client_fd, recv_buffer, MAX_RECV_BUF_LEN - 1, false);
    if (recv_count < 0) return recv_count;
    recv_buffer[recv_count] = '\0';
    discard_all(client_fd, length - (MAX_RECV_BUF_LEN - 1), false);
    return recv_count;
  }
  return recv_all(client_fd, recv_buffer, length, false);
}

// Reads a networked string into recv_buffer
void readString (int client_fd) {
  recv_count = readLengthPrefixedData(client_fd);
  if (recv_count < 0) return;
  recv_buffer[recv_count] = '\0';
}
// Reads a networked string of up to N bytes into recv_buffer
void readStringN (int client_fd, uint32_t max_length) {
  // Forward to readString if max length is invalid
  if (max_length >= MAX_RECV_BUF_LEN) {
    readString(client_fd);
    return;
  }
  // Attempt to read full string within maximum
  uint32_t length = readVarInt(client_fd);
  if (length == (uint32_t)VARNUM_ERROR) {
    recv_count = -1;
    return;
  }
  if (max_length > length) {
    recv_count = recv_all(client_fd, recv_buffer, length, false);
    if (recv_count < 0) return;
    recv_buffer[recv_count] = '\0';
    return;
  }
  // Read string up to maximum, dump the rest
  recv_count = recv_all(client_fd, recv_buffer, max_length, false);
  if (recv_count < 0) return;
  recv_buffer[recv_count] = '\0';
  discard_all(client_fd, length - max_length, false);
}

uint32_t fast_rand () {
  rng_seed ^= rng_seed << 13;
  rng_seed ^= rng_seed >> 17;
  rng_seed ^= rng_seed << 5;
  return rng_seed;
}

uint64_t splitmix64 (uint64_t state) {
  uint64_t z = state + 0x9e3779b97f4a7c15;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
  z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
  return z ^ (z >> 31);
}

#ifndef ESP_PLATFORM
// Returns system time in microseconds.
// On ESP-IDF, this is available in "esp_timer.h", and returns time *since
// the start of the program*, and NOT wall clock time. To ensure
// compatibility, this should only be used to measure time intervals.
int64_t get_program_time () {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}
#endif
