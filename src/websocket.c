#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef ESP_PLATFORM
  #include "lwip/sockets.h"
#else
  #ifdef _WIN32
    #include <winsock2.h>
  #else
    #include <sys/socket.h>
  #endif
  #include <unistd.h>
#endif

#include "globals.h"
#include "tools.h"
#include "websocket.h"
#include "procedures.h"

#define WS_HTTP_BUF 2048
#define WS_IN_BUF 512
#define WS_MAX_CLIENTS MAX_PLAYERS

typedef struct {
  int fd;
  uint8_t mode;
  int64_t accepted_at;
  int http_len;
  uint16_t in_len;
  uint16_t in_pos;
  uint8_t in_buf[WS_IN_BUF];
  uint8_t hdr[14];
  int hdr_len;
  int hdr_need;
  uint64_t payload_left;
  uint8_t mask[4];
  int mask_pos;
  uint8_t masked;
  uint8_t http_buf[WS_HTTP_BUF];
} WsClient;

static WsClient ws_clients[WS_MAX_CLIENTS];

static WsClient *ws_find (int client_fd) {
  for (int i = 0; i < WS_MAX_CLIENTS; i ++) {
    if (ws_clients[i].fd == client_fd) return &ws_clients[i];
  }
  return NULL;
}

int ws_mode (int client_fd) {
  WsClient *c = ws_find(client_fd);
  if (!c) return WS_MODE_RAW;
  return c->mode;
}

int ws_is_open (int client_fd) {
  return ws_mode(client_fd) == WS_MODE_OPEN;
}

void ws_client_init (int client_fd) {
  WsClient *c = ws_find(client_fd);
  if (!c) {
    // Static array is zero-initialised, so unused slots have fd==0
    // (fd 0 is stdin, never a real client socket).
    for (int i = 0; i < WS_MAX_CLIENTS; i ++) {
      if (ws_clients[i].fd <= 0) { c = &ws_clients[i]; break; }
    }
  }
  if (!c) return;
  memset(c, 0, sizeof(*c));
  c->fd = client_fd;
  c->mode = WS_MODE_UNKNOWN;
  c->accepted_at = get_program_time();
}

void ws_client_close (int client_fd) {
  WsClient *c = ws_find(client_fd);
  if (!c) return;
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

static ssize_t ws_raw_read (WsClient *c, void *buf, size_t n, int blocking) {
  int64_t deadline = get_program_time();
  while (1) {
    ssize_t r = recv(c->fd, buf, n, 0);
    if (r > 0) return r;
    if (r == 0) return -1;
    #ifdef _WIN32
      int err = WSAGetLastError();
      if (err != WSAEWOULDBLOCK && err != WSAEINTR) return -1;
    #else
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
    #endif
    if (!blocking) return 0;
    if (get_program_time() - deadline > RECV_STALL_TIMEOUT_TIME) return -1;
    task_yield();
  }
}

static ssize_t ws_raw_write (WsClient *c, const void *buf, size_t n) {
  const uint8_t *p = (const uint8_t *) buf;
  size_t sent = 0;
  int64_t deadline = get_program_time();
  while (sent < n) {
    #ifdef _WIN32
      ssize_t r = send(c->fd, p + sent, n - sent, 0);
    #else
      ssize_t r = send(c->fd, p + sent, n - sent, MSG_NOSIGNAL);
    #endif
    if (r > 0) { sent += (size_t) r; deadline = get_program_time(); continue; }
    if (r == 0) return -1;
    #ifdef _WIN32
      int err = WSAGetLastError();
      if (err != WSAEWOULDBLOCK && err != WSAEINTR) return -1;
    #else
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
    #endif
    if (get_program_time() - deadline > SEND_STALL_TIMEOUT_TIME) return -1;
    task_yield();
  }
  return (ssize_t) sent;
}

static uint32_t rotl32 (uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

static void sha1_block (uint32_t *h, const uint8_t *block) {
  uint32_t w[80];
  for (int i = 0; i < 16; i ++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (int i = 16; i < 80; i ++) {
    w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i ++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else { f = b ^ c ^ d; k = 0xCA62C1D6; }
    uint32_t t = rotl32(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rotl32(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1 (const uint8_t *data, size_t len, uint8_t out[20]) {
  uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
  size_t i = 0;
  for (; i + 64 <= len; i += 64) sha1_block(h, data + i);

  uint8_t tail[128];
  size_t rem = len - i;
  size_t tail_len = (rem + 9 <= 64) ? 64 : 128;
  memset(tail, 0, tail_len);
  memcpy(tail, data + i, rem);
  tail[rem] = 0x80;
  uint64_t bits = (uint64_t) len * 8;
  for (int j = 0; j < 8; j ++) tail[tail_len - 1 - j] = (uint8_t)(bits >> (8 * j));
  for (size_t k = 0; k < tail_len; k += 64) sha1_block(h, tail + k);

  for (int j = 0; j < 5; j ++) {
    out[j * 4] = (uint8_t)(h[j] >> 24);
    out[j * 4 + 1] = (uint8_t)(h[j] >> 16);
    out[j * 4 + 2] = (uint8_t)(h[j] >> 8);
    out[j * 4 + 3] = (uint8_t) h[j];
  }
}

static int base64_encode (const uint8_t *in, int len, char *out) {
  static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int o = 0, i = 0;
  while (i + 3 <= len) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = tab[(v >> 18) & 63];
    out[o++] = tab[(v >> 12) & 63];
    out[o++] = tab[(v >> 6) & 63];
    out[o++] = tab[v & 63];
    i += 3;
  }
  int rem = len - i;
  if (rem == 1) {
    uint32_t v = (uint32_t) in[i] << 16;
    out[o++] = tab[(v >> 18) & 63];
    out[o++] = tab[(v >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
  } else if (rem == 2) {
    uint32_t v = ((uint32_t) in[i] << 16) | ((uint32_t) in[i + 1] << 8);
    out[o++] = tab[(v >> 18) & 63];
    out[o++] = tab[(v >> 12) & 63];
    out[o++] = tab[(v >> 6) & 63];
    out[o++] = '=';
  }
  out[o] = '\0';
  return o;
}

static int ci_equal (const char *a, const char *b, int n) {
  for (int i = 0; i < n; i ++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
    if (x != y) return 0;
  }
  return 1;
}

static int ci_contains (const char *hay, int hay_len, const char *needle) {
  int n = (int) strlen(needle);
  for (int i = 0; i + n <= hay_len; i ++) {
    if (ci_equal(hay + i, needle, n)) return 1;
  }
  return 0;
}

// Copies the value of a header line into out (NUL terminated).
static int header_value (const char *req, int len, const char *name, char *out, int out_max) {
  int name_len = (int) strlen(name);
  int line_start = 0;
  while (line_start < len) {
    int line_end = line_start;
    while (line_end + 1 < len && !(req[line_end] == '\r' && req[line_end + 1] == '\n')) line_end ++;
    if (line_end + 1 >= len) return -1;
    int content_end = line_end;
    int colon = -1;
    for (int i = line_start; i < content_end; i ++) {
      if (req[i] == ':') { colon = i; break; }
    }
    if (colon != -1 && colon - line_start == name_len && ci_equal(req + line_start, name, name_len)) {
      int v = colon + 1;
      while (v < content_end && (req[v] == ' ' || req[v] == '\t')) v ++;
      int n = content_end - v;
      if (n >= out_max) n = out_max - 1;
      memcpy(out, req + v, (size_t) n);
      out[n] = '\0';
      return n;
    }
    line_start = line_end + 2;
  }
  return -1;
}

static int http_end_found (const char *buf, int len) {
  for (int i = 0; i + 3 < len; i ++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return 1;
  }
  return 0;
}

static int send_http_error (WsClient *c, const char *status) {
  char resp[160];
  int n = snprintf(resp, sizeof(resp),
    "HTTP/1.1 %s\r\nConnection: close\r\nContent-Length: 0\r\n"
    "Sec-WebSocket-Version: 13\r\n\r\n", status);
  if (n < 0 || n >= (int) sizeof(resp)) return -1;
  return ws_raw_write(c, resp, (size_t) n) < 0 ? -1 : -1;
}

int ws_sniff (int client_fd) {
  WsClient *c = ws_find(client_fd);
  if (!c) return -1;
  if (c->mode != WS_MODE_UNKNOWN) return 1;

  uint8_t peek[4];
  ssize_t n = recv(client_fd, peek, sizeof(peek), MSG_PEEK);
  #ifdef _WIN32
    if (n == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err != WSAEWOULDBLOCK && err != WSAEINTR) return -1;
      n = 0;
    }
  #else
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
    if (n < 0) n = 0;
  #endif
  if (n == 0) {
    if (get_program_time() - c->accepted_at > NETWORK_TIMEOUT_TIME) return -1;
    return 0;
  }
  if (n < 4) {
    if (get_program_time() - c->accepted_at > NETWORK_TIMEOUT_TIME) return -1;
    return 0;
  }
  if (peek[0] == 'G' && peek[1] == 'E' && peek[2] == 'T' && peek[3] == ' ') {
    c->mode = WS_MODE_HANDSHAKE;
  } else {
    c->mode = WS_MODE_RAW;
  }
  return 1;
}

int ws_handshake_step (int client_fd) {
  WsClient *c = ws_find(client_fd);
  if (!c || c->mode != WS_MODE_HANDSHAKE) return -1;

  while (c->http_len < WS_HTTP_BUF) {
    ssize_t n = ws_raw_read(c, c->http_buf + c->http_len,
                            (size_t)(WS_HTTP_BUF - c->http_len), 0);
    if (n < 0) return -1;
    if (n == 0) break;
    c->http_len += (int) n;
    if (http_end_found((const char *) c->http_buf, c->http_len)) break;
  }
  if (!http_end_found((const char *) c->http_buf, c->http_len)) {
    if (c->http_len >= WS_HTTP_BUF) {
      send_http_error(c, "431 Request Header Fields Too Large");
      return -1;
    }
    return 0;
  }

  char upgrade[64], connection[64], key[128], protocol[128], host[128];
  upgrade[0] = connection[0] = key[0] = protocol[0] = host[0] = '\0';
  header_value((const char *) c->http_buf, c->http_len, "upgrade", upgrade, sizeof(upgrade));
  header_value((const char *) c->http_buf, c->http_len, "connection", connection, sizeof(connection));
  int key_len = header_value((const char *) c->http_buf, c->http_len, "sec-websocket-key", key, sizeof(key));
  header_value((const char *) c->http_buf, c->http_len, "sec-websocket-protocol", protocol, sizeof(protocol));
  header_value((const char *) c->http_buf, c->http_len, "host", host, sizeof(host));

  if (key_len <= 0 || !ci_contains(upgrade, (int) strlen(upgrade), "websocket") ||
      !ci_contains(connection, (int) strlen(connection), "upgrade")) {
    char target_host[160];
    if (host[0] && strchr(host, ':') == NULL) {
      snprintf(target_host, sizeof(target_host), "%s:25565", host);
    } else if (host[0]) {
      snprintf(target_host, sizeof(target_host), "%s", host);
    } else {
      snprintf(target_host, sizeof(target_host), "localhost:25565");
    }
    char resp[384];
    int n = snprintf(resp, sizeof(resp),
      "HTTP/1.1 302 Found\r\n"
      "Location: https://mcraft.fun/?ip=ws://%s\r\n"
      "Connection: close\r\n"
      "Content-Length: 0\r\n\r\n", target_host);
    if (n > 0 && n < (int) sizeof(resp)) ws_raw_write(c, resp, (size_t) n);
    return -1;
  }

  char accept_src[160];
  int src_len = snprintf(accept_src, sizeof(accept_src),
                         "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
  if (src_len < 0 || src_len >= (int) sizeof(accept_src)) {
    send_http_error(c, "400 Bad Request");
    return -1;
  }
  uint8_t digest[20];
  sha1((const uint8_t *) accept_src, (size_t) src_len, digest);
  char accept_b64[32];
  base64_encode(digest, 20, accept_b64);

  char resp[384];
  int n;
  if (protocol[0]) {
    char first_proto[64];
    int plen = 0;
    while (protocol[plen] && protocol[plen] != ',' && plen < (int) sizeof(first_proto) - 1) {
      first_proto[plen] = protocol[plen];
      plen ++;
    }
    first_proto[plen] = '\0';
    n = snprintf(resp, sizeof(resp),
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n"
      "Sec-WebSocket-Protocol: %s\r\n\r\n",
      accept_b64, first_proto);
  } else {
    n = snprintf(resp, sizeof(resp),
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n\r\n", accept_b64);
  }
  if (n < 0 || n >= (int) sizeof(resp)) return -1;
  if (ws_raw_write(c, resp, (size_t) n) < 0) return -1;

  // Preserve any extra bytes that arrived after the HTTP header (e.g., Minecraft handshake)
  int header_end = -1;
  for (int i = 0; i + 3 < c->http_len; i++) {
    if (c->http_buf[i] == '\r' && c->http_buf[i + 1] == '\n' &&
        c->http_buf[i + 2] == '\r' && c->http_buf[i + 3] == '\n') {
      header_end = i + 4;
      break;
    }
  }

  c->in_len = 0;
  c->in_pos = 0;
  if (header_end != -1 && c->http_len > header_end) {
    int extra_len = c->http_len - header_end;
    if (extra_len > WS_IN_BUF) extra_len = WS_IN_BUF;
    memcpy(c->in_buf, c->http_buf + header_end, extra_len);
    c->in_len = extra_len;
  }

  c->http_len = 0;
  c->mode = WS_MODE_OPEN;
  printf("WebSocket handshake complete (fd %d, protocol: %s)\n",
         client_fd, protocol[0] ? protocol : "none");
  return 1;
}

// Reads and decodes frames until payload bytes or a control frame is handled.
// Returns 1 when in_buf has bytes, 0 when nothing is available right now and
// -1 when the peer closed or the stream is invalid.
static int ws_fill (WsClient *c, int blocking) {
  while (1) {
    if (c->in_pos >= c->in_len) {
      c->in_pos = 0;
      c->in_len = 0;
    }

    if (c->payload_left > 0) {
      size_t space = WS_IN_BUF - c->in_len;
      if (space == 0) return 1;
      size_t want = c->payload_left < (uint64_t) space ? (size_t) c->payload_left : space;
      ssize_t n = ws_raw_read(c, c->in_buf + c->in_len, want, blocking);
      if (n < 0) return -1;
      if (n == 0) return c->in_len ? 1 : 0;
      if (c->masked) {
        for (ssize_t k = 0; k < n; k ++) {
          c->in_buf[c->in_len + k] ^= c->mask[(c->mask_pos + k) & 3];
        }
      }
      c->mask_pos = (c->mask_pos + (int) n) & 3;
      c->in_len = (uint16_t)(c->in_len + n);
      c->payload_left -= (uint64_t) n;
      return 1;
    }

    if (c->hdr_len < 2) {
      ssize_t n = ws_raw_read(c, c->hdr + c->hdr_len, (size_t)(2 - c->hdr_len), blocking);
      if (n < 0) return -1;
      if (n == 0) return c->in_len ? 1 : 0;
      c->hdr_len += (int) n;
      if (c->hdr_len < 2) continue;
    }

    if (c->hdr_need == 0) {
      int len7 = c->hdr[1] & 0x7F;
      int need = 2;
      if (len7 == 126) need += 2;
      else if (len7 == 127) need += 8;
      if (c->hdr[1] & 0x80) need += 4;
      c->hdr_need = need;
    }

    if (c->hdr_len < c->hdr_need) {
      ssize_t n = ws_raw_read(c, c->hdr + c->hdr_len,
                              (size_t)(c->hdr_need - c->hdr_len), blocking);
      if (n < 0) return -1;
      if (n == 0) return c->in_len ? 1 : 0;
      c->hdr_len += (int) n;
      if (c->hdr_len < c->hdr_need) continue;
    }

    uint8_t b0 = c->hdr[0];
    uint8_t b1 = c->hdr[1];
    int opcode = b0 & 0x0F;
    int fin = (b0 & 0x80) != 0;
    int masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;
    int p = 2;
    if (len == 126) {
      len = ((uint64_t) c->hdr[2] << 8) | c->hdr[3];
      p = 4;
    } else if (len == 127) {
      len = 0;
      for (int i = 0; i < 8; i ++) len = (len << 8) | c->hdr[2 + i];
      p = 10;
    }
    if (masked) {
      memcpy(c->mask, c->hdr + p, 4);
      p += 4;
    }
    c->masked = (uint8_t) masked;
    c->mask_pos = 0;
    c->hdr_len = 0;
    c->hdr_need = 0;

    if (b0 & 0x70) return -1;

    if (opcode >= 0x8) {
      if (!fin || len > 125) return -1;
      uint8_t ctl[125];
      size_t got = 0;
      while (got < len) {
        ssize_t n = ws_raw_read(c, ctl + got, (size_t) len - got, 1);
        if (n <= 0) return -1;
        got += (size_t) n;
      }
      if (masked) {
        for (size_t k = 0; k < len; k ++) ctl[k] ^= c->mask[k & 3];
      }
      if (opcode == 0x8) return -1;
      if (opcode == 0x9) {
        uint8_t frame[2 + 125];
        frame[0] = 0x8A;
        frame[1] = (uint8_t) len;
        memcpy(frame + 2, ctl, (size_t) len);
        if (ws_raw_write(c, frame, (size_t) len + 2) < 0) return -1;
      }
      continue;
    }

    c->payload_left = len;
  }
}

int ws_poll (int client_fd) {
  WsClient *c = ws_find(client_fd);
  if (!c || c->mode != WS_MODE_OPEN) return -1;
  if (c->in_pos < c->in_len) return 1;
  int r = ws_fill(c, 0);
  if (r < 0) return -1;
  return r ? 1 : 0;
}

ssize_t ws_recv (int client_fd, void *buf, size_t n, int require_first) {
  WsClient *c = ws_find(client_fd);
  if (!c || c->mode != WS_MODE_OPEN) return -1;

  size_t done = 0;
  while (done < n) {
    if (c->in_pos >= c->in_len) {
      int r = ws_fill(c, 0);
      if (r < 0) {
        recv_count = done ? (ssize_t) done : -1;
        return done ? (ssize_t) done : -1;
      }
      if (c->in_pos >= c->in_len) {
        if (require_first && done == 0) return 0;
        task_yield();
        continue;
      }
    }
    size_t avail = (size_t)(c->in_len - c->in_pos);
    size_t take = (n - done) < avail ? (n - done) : avail;
    memcpy((uint8_t *) buf + done, c->in_buf + c->in_pos, take);
    c->in_pos = (uint16_t)(c->in_pos + take);
    done += take;
  }
  total_bytes_received += done;
  return (ssize_t) done;
}

int ws_frame_header (uint8_t *out, uint64_t len) {
  out[0] = 0x82;
  if (len < 126) {
    out[1] = (uint8_t) len;
    return 2;
  }
  if (len <= 0xFFFF) {
    out[1] = 126;
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t) len;
    return 4;
  }
  out[1] = 127;
  for (int i = 0; i < 8; i ++) out[2 + i] = (uint8_t)(len >> (56 - 8 * i));
  return 10;
}
