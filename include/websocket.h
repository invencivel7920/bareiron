#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stdint.h>
#include <stddef.h>

// Client transport modes. Every accepted socket starts as unknown and is
// classified on its first bytes: an HTTP upgrade request becomes a WebSocket,
// anything else is treated as raw Minecraft protocol.
#define WS_MODE_UNKNOWN 0
#define WS_MODE_RAW 1
#define WS_MODE_HANDSHAKE 2
#define WS_MODE_OPEN 3

void ws_client_init (int client_fd);
void ws_client_close (int client_fd);
int ws_mode (int client_fd);
int ws_is_open (int client_fd);

// Returns 1 when the transport is decided, 0 when more bytes are needed and
// -1 on a closed/errored socket.
int ws_sniff (int client_fd);

// Drives the HTTP upgrade. Returns 0 while incomplete, 1 when switched to
// WebSocket and -1 when rejected or failed.
int ws_handshake_step (int client_fd);

// Non-blocking readiness check for decoded WebSocket payload.
// Returns 1 when at least one byte is available, 0 otherwise, -1 on close.
int ws_poll (int client_fd);

// Reads decoded payload bytes, mirroring recv_all semantics.
ssize_t ws_recv (int client_fd, void *buf, size_t n, int require_first);

// Writes a binary frame header for len payload bytes.
// Returns the header length written into out (out must hold 10 bytes).
int ws_frame_header (uint8_t *out, uint64_t len);

#endif
