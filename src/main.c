#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>



#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif


#ifdef ESP_PLATFORM
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "nvs_flash.h"
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_netif.h"
  #include "esp_timer.h"
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
#else
  #include <sys/types.h>
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "packets.h"
#include "worldgen.h"
#include "registries.h"
#include "procedures.h"
#include "websocket.h"
#include "serialize.h"

// ============================================================
//  MACROS DE PERFORMANCE (renomeadas para evitar conflito)
// ============================================================
#define BAREIRON_PERF_START()   int64_t _perf_start = GET_TIME()
#define BAREIRON_PERF_END(msg)  printf("[PERF] %s: %lld us\n", msg, GET_TIME() - _perf_start)

// Flag global de debug (visível em outros arquivos)
bool debug_mode = false;
// Variáveis estáticas para reduzir uso de pilha
static uint8_t uuid[16];
static char name[16];
static int login_ack_pending[1024];
static int config_sent[1024];

#define CHUNK_QUEUE_SIZE 128
#define CHUNK_SEND_INTERVAL_US 20000
typedef struct { int fd; short cx, cz; } ChunkJob;
static ChunkJob chunk_queue[CHUNK_QUEUE_SIZE];
static int chunk_queue_head = 0, chunk_queue_tail = 0;
static int64_t chunk_send_deadline = 0;

static void enqueue_chunk (int fd, short cx, short cz) {
  int next = (chunk_queue_tail + 1) % CHUNK_QUEUE_SIZE;
  if (next == chunk_queue_head) return;
  chunk_queue[chunk_queue_tail].fd = fd;
  chunk_queue[chunk_queue_tail].cx = cx;
  chunk_queue[chunk_queue_tail].cz = cz;
  chunk_queue_tail = next;
}

static void process_chunk_queue (void) {
  if (chunk_queue_head == chunk_queue_tail) return;
  if (get_program_time() < chunk_send_deadline) return;
  ChunkJob *job = &chunk_queue[chunk_queue_head];
  chunk_queue_head = (chunk_queue_head + 1) % CHUNK_QUEUE_SIZE;
  if (getClientState(job->fd) != STATE_PLAY) return;
  sc_chunkDataAndUpdateLight(job->fd, job->cx, job->cz);
  flush_send_all();
  chunk_send_deadline = get_program_time() + CHUNK_SEND_INTERVAL_US;
}

// Declaração da função handlePacket (definida abaixo)
void handlePacket(int client_fd, int length, int packet_id, int state);

// Substituição de get_program_time por esp_timer_get_time no ESP32
#ifdef ESP_PLATFORM
  #define GET_TIME() esp_timer_get_time()
#else
  // Para outras plataformas, mantém a função original (não mostrada, mas definida em tools.c)
  extern int64_t get_program_time();
  #define GET_TIME() get_program_time()
#endif

/**
 * Routes an incoming packet to its packet handler or procedure.
 * (mantido, mas com variáveis estáticas e sem logs de desenvolvimento)
 */
void handlePacket(int client_fd, int length, int packet_id, int state) {
  uint64_t bytes_received_start = total_bytes_received;

  switch (packet_id) {

    case 0x00:
      if (state == STATE_NONE) {
        if (cs_handshake(client_fd)) break;
      } else if (state == STATE_STATUS) {
        if (sc_statusResponse(client_fd)) break;
      } if (state == STATE_LOGIN) {
        if (cs_loginStart(client_fd, uuid, name)) break;
        if (reservePlayerData(client_fd, uuid, name)) {
          recv_count = 0;
          return;
        }
        if (sc_loginSuccess(client_fd, uuid, name)) break;
        setClientState(client_fd, STATE_CONFIGURATION);
        if (client_fd >= 0 && client_fd < 1024) login_ack_pending[client_fd] = 1;
      } else if (state == STATE_CONFIGURATION) {
        if (length == 0) break;
        if (cs_clientInformation(client_fd)) break;
        if (client_fd >= 0 && client_fd < 1024 && config_sent[client_fd]) break;
        if (sc_featureFlags(client_fd)) break;
        if (sc_updateTags(client_fd)) break;
        if (sc_knownPacks(client_fd)) break;

        #ifdef SEND_BRAND
        if (sc_sendPluginMessage(client_fd, "minecraft:brand", (uint8_t *)brand, brand_len)) break;
        #endif
        if (client_fd >= 0 && client_fd < 1024) config_sent[client_fd] = 1;
      }
      break;

    case 0x01:
      if (state == STATE_STATUS) {
        uint64_t payload = readUint64(client_fd);
        writeByte(client_fd, 9);
        writeByte(client_fd, 0x01);
        writeUint64(client_fd, payload);
        recv_count = 0;
        return;
      }
      break;

    case 0x02:
      if (state == STATE_CONFIGURATION) cs_pluginMessage(client_fd);
      break;

    case 0x03:
      if (state == STATE_LOGIN) {
        printf("Client Acknowledged Login\n\n");
        setClientState(client_fd, STATE_CONFIGURATION);
        if (client_fd >= 0 && client_fd < 1024) login_ack_pending[client_fd] = 0;
      } else if (state == STATE_CONFIGURATION && client_fd >= 0 && client_fd < 1024 && login_ack_pending[client_fd]) {
        printf("Client Acknowledged Login (late)\n\n");
        login_ack_pending[client_fd] = 0;
      } else if (state == STATE_CONFIGURATION) {
        printf("Client Acknowledged Configuration\n\n");
        setClientState(client_fd, STATE_PLAY);
        sc_loginPlay(client_fd);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;
        spawnPlayer(player);

        sc_setCenterChunk(client_fd, 0, 0);
        enqueue_chunk(client_fd, 0, 0);
        for (int r = 1; r <= VIEW_DISTANCE; r++) {
          for (int cx = -r; cx <= r; cx++) {
            for (int cz = -r; cz <= r; cz++) {
              if (abs(cx) == r || abs(cz) == r) enqueue_chunk(client_fd, cx, cz);
            }
          }
        }
        player->visited_x[VISITED_HISTORY - 1] = 0;
        player->visited_z[VISITED_HISTORY - 1] = 0;

        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (player_data[i].client_fd == -1) continue;
          if (player_data[i].flags & 0x20) continue;
          sc_playerInfoUpdateAddPlayer(client_fd, player_data[i]);
          sc_spawnEntityPlayer(client_fd, player_data[i]);
        }

        uint8_t uuid_mob[16];
        uint32_t r = fast_rand();
        memcpy(uuid_mob, &r, 4);
        for (int i = 0; i < MAX_MOBS; i ++) {
          if (mob_data[i].type == 0) continue;
          if ((mob_data[i].data & 31) == 0) continue;
          memcpy(uuid_mob + 4, &i, 4);
          sc_spawnEntity(
            client_fd, -2 - i, uuid_mob,
            mob_data[i].type, mob_data[i].x, mob_data[i].y, mob_data[i].z,
            0, 0
          );
          broadcastMobMetadata(client_fd, -2 - i);
        }

      }
      break;

    case 0x07:
      if (state == STATE_CONFIGURATION) {
        printf("  Serverbound Known Packs received\n");
        if (cs_knownPacks(client_fd)) { printf("  cs_knownPacks FAILED\n"); break; }
        printf("  Sending registries\n");
        if (sc_registries(client_fd)) { printf("  sc_registries FAILED\n"); break; }
        printf("  Finishing configuration\n\n");
        if (sc_finishConfiguration(client_fd)) { printf("  sc_finishConfiguration FAILED\n"); break; }
      }
      break;

    case 0x08:
      if (state == STATE_PLAY) cs_chat(client_fd);
      break;

    case 0x0B:
      if (state == STATE_PLAY) cs_clientStatus(client_fd);
      break;

    case 0x0C: // Client tick (ignored)
      break;

    case 0x11:
      if (state == STATE_PLAY) cs_clickContainer(client_fd);
      break;

    case 0x12:
      if (state == STATE_PLAY) cs_closeContainer(client_fd);
      break;

    case 0x1B:
      if (state == STATE_PLAY) {
        discard_all(client_fd, length, false);
      }
      break;

    case 0x19:
      if (state == STATE_PLAY) cs_interact(client_fd);
      break;

    case 0x1D:
    case 0x1E:
    case 0x1F:
    case 0x20:
      if (state == STATE_PLAY) {

        double x, y, z;
        float yaw, pitch;
        uint8_t on_ground;

        if (packet_id == 0x1D) cs_setPlayerPosition(client_fd, &x, &y, &z, &on_ground);
        else if (packet_id == 0x1F) cs_setPlayerRotation (client_fd, &yaw, &pitch, &on_ground);
        else if (packet_id == 0x20) cs_setPlayerMovementFlags (client_fd, &on_ground);
        else cs_setPlayerPositionAndRotation(client_fd, &x, &y, &z, &yaw, &pitch, &on_ground);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        // Reject impossible positions (NaN/inf/huge) and long teleports: a
        // single forged packet could otherwise trigger thousands of chunk
        // packets in one tick and stall the server.
        if (packet_id == 0x1D || packet_id == 0x1E) {
          if (!(x > -3.0e7 && x < 3.0e7 && z > -3.0e7 && z < 3.0e7 && y > -64 && y < 1024)) break;
          if (x - player->x > 16 || x - player->x < -16 ||
              z - player->z > 16 || z - player->z < -16 ||
              y - player->y > 32 || y - player->y < -32) {
            sc_synchronizePlayerPosition(client_fd, player->x, player->y, player->z,
                                         player->yaw * 180 / 127, player->pitch * 90 / 127);
            break;
          }
        }

        uint8_t block_feet = getBlockAt(player->x, player->y, player->z);
        uint8_t swimming = block_feet >= B_water && block_feet < B_water + 8;

        if (on_ground) {
          int16_t damage = player->grounded_y - player->y - 3;
          if (damage > 0 && (GAMEMODE == 0 || GAMEMODE == 2) && !swimming) {
            hurtEntity(client_fd, -1, D_fall, damage);
          }
          player->grounded_y = player->y;
        } else if (swimming) {
          player->grounded_y = player->y;
        }

        if (packet_id == 0x20) break;

        if (packet_id != 0x1D) {
          player->yaw = ((short)(yaw + 540) % 360 - 180) * 127 / 180;
          player->pitch = pitch / 90.0f * 127.0f;
        }

        uint8_t should_broadcast = true;

        #ifndef BROADCAST_ALL_MOVEMENT
          should_broadcast = !(player->flags & 0x40);
          if (should_broadcast) player->flags |= 0x40;
        #endif

        #ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
          if (++player->packets_since_update < client_count) {
            should_broadcast = false;
          } else {
            player->packets_since_update = 0;
          }
        #endif

        if (should_broadcast) {
          if (packet_id == 0x1D) {
            yaw = player->yaw * 180 / 127;
            pitch = player->pitch * 90 / 127;
          }
          for (int i = 0; i < MAX_PLAYERS; i ++) {
            if (!isPlayClient(player_data[i].client_fd)) continue;
            if (player_data[i].flags & 0x20) continue;
            if (player_data[i].client_fd == client_fd) continue;
            if (packet_id == 0x1F) {
              sc_updateEntityRotation(player_data[i].client_fd, client_fd, player->yaw, player->pitch);
            } else {
              sc_teleportEntity(player_data[i].client_fd, client_fd, x, y, z, yaw, pitch);
            }
            sc_setHeadRotation(player_data[i].client_fd, client_fd, player->yaw);
          }
        }

        if (packet_id == 0x1F) break;

        if (player->saturation == 0) {
          if (player->hunger > 0) player->hunger--;
          player->saturation = 200;
          sc_setHealth(client_fd, player->health, player->hunger, player->saturation);
        } else if (player->flags & 0x08) {
          player->saturation -= 1;
        }

        short cx = x, cy = y, cz = z;
        if (x < 0) cx -= 1;
        if (z < 0) cz -= 1;
        short _x = (cx < 0 ? cx - 16 : cx) / 16, _z = (cz < 0 ? cz - 16 : cz) / 16;
        short dx = _x - (player->x < 0 ? player->x - 16 : player->x) / 16;
        short dz = _z - (player->z < 0 ? player->z - 16 : player->z) / 16;

        if (cy < 0) {
          cy = 0;
          player->grounded_y = 0;
          sc_synchronizePlayerPosition(client_fd, cx, 0, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        } else if (cy > 255) {
          cy = 255;
          sc_synchronizePlayerPosition(client_fd, cx, 255, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        }

        player->x = cx;
        player->y = cy;
        player->z = cz;

        if (dx == 0 && dz == 0) break;

        int found = false;
        for (int i = 0; i < VISITED_HISTORY; i ++) {
          if (player->visited_x[i] == _x && player->visited_z[i] == _z) {
            found = true;
            break;
          }
        }
        if (found) break;

        for (int i = 0; i < VISITED_HISTORY - 1; i ++) {
          player->visited_x[i] = player->visited_x[i + 1];
          player->visited_z[i] = player->visited_z[i + 1];
        }
        player->visited_x[VISITED_HISTORY - 1] = _x;
        player->visited_z[VISITED_HISTORY - 1] = _z;

        uint32_t r = fast_rand();
        if ((r & 3) == 0) {
          short mob_dx_sign = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
          short mob_dz_sign = (dz > 0) ? 1 : (dz < 0 ? -1 : 0);
          short mob_x = (_x + mob_dx_sign * VIEW_DISTANCE) * 16 + ((r >> 4) & 15);
          short mob_z = (_z + mob_dz_sign * VIEW_DISTANCE) * 16 + ((r >> 8) & 15);
          int mob_y = (int)cy - 8;
          if (mob_y < 1) mob_y = 1;
          uint8_t b_low = getBlockAt(mob_x, mob_y - 1, mob_z);
          uint8_t b_mid = getBlockAt(mob_x, mob_y, mob_z);
          uint8_t b_top = getBlockAt(mob_x, mob_y + 1, mob_z);
          while (mob_y < 80) {
            if (!isPassableBlock(b_low) && isPassableSpawnBlock(b_mid) && isPassableSpawnBlock(b_top)) break;
            b_low = b_mid;
            b_mid = b_top;
            b_top = getBlockAt(mob_x, mob_y + 2, mob_z);
            mob_y ++;
          }
          if (mob_y < 80) {
            if ((world_time < 13000 || world_time > 23460) && mob_y > 48) {
              uint32_t mob_choice = (r >> 12) & 3;
              if (mob_choice == 0) spawnMob(25, mob_x, mob_y, mob_z, 4);
              else if (mob_choice == 1) spawnMob(28, mob_x, mob_y, mob_z, 10);
              else if (mob_choice == 2) spawnMob(95, mob_x, mob_y, mob_z, 10);
              else if (mob_choice == 3) spawnMob(106, mob_x, mob_y, mob_z, 8);
            } else {
              spawnMob(145, mob_x, mob_y, mob_z, 20);
            }
          }
        }

        sc_setCenterChunk(client_fd, _x, _z);

        int dx_abs = dx > 0 ? dx : -dx;
        int dx_sign = dx > 0 ? 1 : -1;
        for (int i = 1; i <= dx_abs; i++) {
          int strip_x = _x + dx_sign * (VIEW_DISTANCE - dx_abs + i);
          enqueue_chunk(client_fd, strip_x, _z);
          for (int j = 1; j <= VIEW_DISTANCE; j++) {
            enqueue_chunk(client_fd, strip_x, _z - j);
            enqueue_chunk(client_fd, strip_x, _z + j);
          }
        }
        int dz_abs = dz > 0 ? dz : -dz;
        int dz_sign = dz > 0 ? 1 : -1;
        for (int i = 1; i <= dz_abs; i++) {
          int strip_z = _z + dz_sign * (VIEW_DISTANCE - dz_abs + i);
          enqueue_chunk(client_fd, _x, strip_z);
          for (int j = 1; j <= VIEW_DISTANCE; j++) {
            enqueue_chunk(client_fd, _x - j, strip_z);
            enqueue_chunk(client_fd, _x + j, strip_z);
          }
        }

      }
      break;

    case 0x29:
      if (state == STATE_PLAY) cs_playerCommand(client_fd);
      break;

    case 0x2A:
      if (state == STATE_PLAY) cs_playerInput(client_fd);
      break;

    case 0x2B:
      if (state == STATE_PLAY) cs_playerLoaded(client_fd);
      break;

    case 0x34:
      if (state == STATE_PLAY) cs_setHeldItem(client_fd);
      break;

    case 0x3C:
      if (state == STATE_PLAY) cs_swingArm(client_fd);
      break;

    case 0x28:
      if (state == STATE_PLAY) cs_playerAction(client_fd);
      break;

    case 0x3F:
      if (state == STATE_PLAY) cs_useItemOn(client_fd);
      break;

    case 0x40:
      if (state == STATE_PLAY) cs_useItem(client_fd);
      break;

    default:
      // Log removido
      discard_all(client_fd, length, false);
      break;

  }

  int processed_length = total_bytes_received - bytes_received_start;
  if (processed_length == length) return;

  if (length > processed_length) {
    discard_all(client_fd, length - processed_length, false);
  }

  // Logs de discrepância removidos
}

int main () {
  #ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      fprintf(stderr, "WSAStartup failed\n");
      exit(EXIT_FAILURE);
    }
  #endif

  world_seed = splitmix64(world_seed);
  printf("World seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((world_seed >> (8 * i)) & 255));

  rng_seed = splitmix64(rng_seed);
  printf("\nRNG seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((rng_seed >> (8 * i)) & 255));
  printf("\n\n");

  for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
    block_changes[i].block = 0xFF;
  }

  if (initSerializer()) exit(EXIT_FAILURE);

  int clients[MAX_PLAYERS];
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    clients[i] = -1;
    client_states[i * 2] = -1;
    player_data[i].client_fd = -1;
  }
  registerClientSlots(clients);

  int server_fd, opt = 1;
  struct sockaddr_in server_addr, client_addr;
  socklen_t addr_len = sizeof(client_addr);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }
#ifdef _WIN32
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
#else
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif
    perror("socket options failed");
    exit(EXIT_FAILURE);
  }

  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  if (listen(server_fd, 5) < 0) {
    perror("listen failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }
  printf("=== VERSAO NOVA DO CODIGO ===\n");
  printf("Server listening on port %d...\n", PORT);

  #ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(server_fd, FIONBIO, &mode) != 0) {
      fprintf(stderr, "Failed to set non-blocking mode\n");
      exit(EXIT_FAILURE);
    }
  #else
  int flags = fcntl(server_fd, F_GETFL, 0);
  fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
  #endif

  // Usar GET_TIME() macro
  int64_t last_tick_time = GET_TIME();

  while (true) {
    task_yield();
    int64_t now = GET_TIME();
    int64_t wait_us = last_tick_time + TIME_BETWEEN_TICKS - now;
    if (wait_us < 0) wait_us = 0;
    if (chunk_queue_head != chunk_queue_tail) {
      if (get_program_time() >= chunk_send_deadline) {
        if (wait_us > 0) wait_us = 0;
      } else {
        int64_t w = chunk_send_deadline - get_program_time();
        if (w < wait_us) wait_us = w;
      }
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(server_fd, &read_fds);
    int max_fd = server_fd;
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (clients[i] == -1) continue;
      FD_SET(clients[i], &read_fds);
      if (clients[i] > max_fd) max_fd = clients[i];
    }

    struct timeval timeout;
    timeout.tv_sec = wait_us / 1000000;
    timeout.tv_usec = wait_us % 1000000;

    #ifdef _WIN32
    int ready = select(0, &read_fds, NULL, NULL, &timeout);
    if (ready < 0) {
      if (WSAGetLastError() == WSAEINTR) continue;
      perror("select failed");
      break;
    }
    #else
    int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
    if (ready < 0) {
      if (errno == EINTR) continue;
      perror("select failed");
      break;
    }
    #endif

    now = GET_TIME();
    if (now - last_tick_time >= TIME_BETWEEN_TICKS) {
      handleServerTick(now - last_tick_time);
      flush_send_all();
      last_tick_time = now;
    }

    if (FD_ISSET(server_fd, &read_fds)) {
      while (true) {
        int new_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (new_fd == -1) {
          #ifdef _WIN32
          if (WSAGetLastError() != WSAEWOULDBLOCK) perror("accept failed");
          #else
          if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) perror("accept failed");
          #endif
          break;
        }
        int slot = -1;
        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (clients[i] == -1) { slot = i; break; }
        }
        if (slot == -1) {
          #ifdef _WIN32
          closesocket(new_fd);
          #else
          close(new_fd);
          #endif
          continue;
        }
        clients[slot] = new_fd;
        #ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(new_fd, FIONBIO, &mode);
        #else
        int flags = fcntl(new_fd, F_GETFL, 0);
        fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);
        #endif
        int nodelay = 1;
        setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, sizeof(nodelay));
        client_count ++;
        ws_client_init(new_fd);
        if (new_fd >= 0 && new_fd < 1024) {
          login_ack_pending[new_fd] = 0;
          config_sent[new_fd] = 0;
        }
        printf("New client, fd: %d\n", new_fd);
      }
    }

    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (clients[i] == -1) continue;

      int client_fd = clients[i];

      // WebSocket buffer may have pending data even if select() didn't report it
      if (!FD_ISSET(client_fd, &read_fds)) {
        if (ws_mode(client_fd) != WS_MODE_OPEN || ws_poll(client_fd) <= 0) {
          continue;
        }
      }

      int mode = ws_mode(client_fd);
      if (mode == WS_MODE_UNKNOWN) {
        int sniffed = ws_sniff(client_fd);
        if (sniffed < 0) {
          disconnectClient(&clients[i], 9);
          continue;
        }
        if (sniffed == 0) continue;
        mode = ws_mode(client_fd);
      }
      if (mode == WS_MODE_HANDSHAKE) {
        int upgraded = ws_handshake_step(client_fd);
        if (upgraded < 0) {
          disconnectClient(&clients[i], 8);
          continue;
        }
        mode = ws_mode(client_fd);
      }

      int readable = conn_poll_readable(client_fd);
      if (readable < 0) {
        disconnectClient(&clients[i], 1);
        continue;
      }
      if (readable == 0) continue;
      recv_count = 1;

      int length = readVarInt(client_fd);
      if ((uint32_t)length == VARNUM_ERROR) {
        continue;
      }

      int state = getClientState(client_fd);
      if (state == STATE_LOGIN && client_fd >= 0 && client_fd < 1024 && login_ack_pending[client_fd]) {
        state = STATE_CONFIGURATION;
      }

      int packet_id = readVarInt(client_fd);
      if ((uint32_t)packet_id == VARNUM_ERROR) {
        continue;
      }
      length -= sizeVarInt(packet_id);

      if (length < 0 || length > 2097152) {
        disconnectClient(&clients[i], 2);
        continue;
      }

      handlePacket(client_fd, length, packet_id, state);
      flush_send_all();
      if (recv_count == 0 || (recv_count == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        if (state == STATE_STATUS) {
          continue;
        }
        disconnectClient(&clients[i], 4);
        continue;
      }
    }

    process_chunk_queue();
  }

  close(server_fd);

  #ifdef _WIN32
    WSACleanup();
  #endif

  printf("Server closed.\n");
  return 0;
}

#ifdef ESP_PLATFORM

void bareiron_main (void *pvParameters) {
  main();
  vTaskDelete(NULL);
}

static void wifi_event_handler (void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    printf("\nWiFi conectado. IP do servidor: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    printf("No Minecraft 1.21.8, conecte em: " IPSTR ":25565\n\n", IP2STR(&event->ip_info.ip));
    // Pilha de 8192 bytes: main() serializa chunks e pacotes no proprio stack.
    xTaskCreate(bareiron_main, "bareiron", 8192, NULL, 5, NULL);
  }
}

void wifi_init () {
  esp_err_t nvs_ret = nvs_flash_init();
  if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    printf("NVS invalida (0x%X), apagando e reiniciando...\n", nvs_ret);
    nvs_flash_erase();
    nvs_flash_init();
  }
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

  wifi_config_t wifi_config = {
    .sta = {
      .ssid = WIFI_SSID,
      .password = WIFI_PASS,
      .threshold.authmode = WIFI_AUTH_WPA2_PSK
    }
  };

  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_start();
}

void app_main () {

  esp_timer_early_init();
  wifi_init();
}

#endif
