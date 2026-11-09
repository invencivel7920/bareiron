#ifndef H_GLOBALS
#define H_GLOBALS

#include <stdint.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
  #define WIFI_SSID "brisa-383290"
  #define WIFI_PASS "77r1ucvq"
  void task_yield ();
#else
  #define task_yield();
#endif
#include <stdbool.h>
// TCP port, Minecraft's default is 25565
#define PORT 25565

// Quantos jogadores manter em memória (agora 5)
#define MAX_PLAYERS 5

// Quantos mobs alocar (igual ao número de jogadores)
#define MAX_MOBS (MAX_PLAYERS)

// Distância de despawn de mobs
#define MOB_DESPAWN_DISTANCE 256

// Modo de jogo: 2 = aventura (menos dano)
#define GAMEMODE 0

#define VIEW_DISTANCE 3

// Intervalo entre ticks (500 ms para reduzir CPU)
#define TIME_BETWEEN_TICKS 500000

#define TICKS_PER_SECOND ((float)1000000 / TIME_BETWEEN_TICKS)

// Sementes (mantidas)
#define INITIAL_WORLD_SEED 0xA103DE6C
#define INITIAL_RNG_SEED 0xE2B9419

// Tamanho do chunk (não alterar)
#define CHUNK_SIZE 8
#define TERRAIN_BASE_HEIGHT 60
#define CAVE_BASE_DEPTH 24
#define BIOME_SIZE (CHUNK_SIZE * 8)
#define BIOME_RADIUS (BIOME_SIZE / 2)

// Histórico de chunks visitados reduzido para 2
#define VISITED_HISTORY 2

// Limite de alterações de bloco reduzido para 1000
#define MAX_BLOCK_CHANGES 1000



// Intervalo de sincronização (15s)
#define DISK_SYNC_INTERVAL 15000000

// Sincronizar blocos em intervalo (evita escritas constantes)
// #define DISK_SYNC_BLOCKS_ON_INTERVAL

// Timeout de rede reduzido para 10s
#define NETWORK_TIMEOUT_TIME 10000000

// Limite de espera por um pacote parcial: o servidor é single-threaded,
// então um cliente lento não pode travar tick e os demais jogadores.
#define RECV_STALL_TIMEOUT_TIME 5000000
#define SEND_STALL_TIMEOUT_TIME 5000000

// Buffer de recepção (192 suporta Client Information com locale + varints)
#define MAX_RECV_BUF_LEN 192

// Desabilitar funcionalidades pesadas
// #define SEND_BRAND
// #define BROADCAST_ALL_MOVEMENT   // desativado para reduzir tráfego
#define SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT // manter (ajuda com 5 players)

// #define ENABLE_PLAYER_FLIGHT
#define ENABLE_PICKUP_ANIMATION
#define ENABLE_CACTUS_DAMAGE

// Desabilitar logs de desenvolvimento
// #define DEV_LOG_UNKNOWN_PACKETS
// #define DEV_LOG_LENGTH_DISCREPANCY
// #define DEV_LOG_CHUNK_GENERATION
// #define DEV_ENABLE_BEEF_DUMPS

#define STATE_NONE 0
#define STATE_STATUS 1
#define STATE_LOGIN 2
#define STATE_TRANSFER 3
#define STATE_CONFIGURATION 4
#define STATE_PLAY 5

// ... (restante das definições de structs e variáveis externas permanece igual) ...

extern ssize_t recv_count;
extern uint8_t recv_buffer[MAX_RECV_BUF_LEN];
extern uint32_t world_seed;
extern uint32_t rng_seed;
extern uint16_t world_time;
extern uint32_t server_ticks;
extern const char motd[];
extern uint8_t motd_len;
#ifdef SEND_BRAND
  extern const char brand[];
  extern uint8_t brand_len;
#endif
extern uint16_t client_count;

typedef struct {
  short x;
  short z;
  uint8_t y;
  uint8_t block;
} BlockChange;

#pragma pack(push, 1)

typedef struct {
  uint8_t uuid[16];
  char name[16];
  int client_fd;
  short x;
  uint8_t y;
  short z;
  short visited_x[VISITED_HISTORY];
  short visited_z[VISITED_HISTORY];
  #ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
    uint16_t packets_since_update;
  #endif
  int8_t yaw;
  int8_t pitch;
  uint8_t grounded_y;
  uint8_t health;
  uint8_t hunger;
  uint16_t saturation;
  uint8_t hotbar;
  uint16_t inventory_items[41];
  uint16_t craft_items[9];
  uint8_t inventory_count[41];
  uint8_t craft_count[9];
  uint16_t flagval_16;
  uint8_t flagval_8;
  uint8_t flags;
} PlayerData;

typedef struct {
  uint8_t type;
  short x;
  uint8_t y;
  short z;
  uint8_t data;
} MobData;

#pragma pack(pop)

union EntityDataValue {
  uint8_t byte;
  int pose;
};

typedef struct {
  uint8_t index;
  int type;
  union EntityDataValue value;
} EntityData;

extern BlockChange block_changes[MAX_BLOCK_CHANGES];
extern int block_changes_count;
extern PlayerData player_data[MAX_PLAYERS];
extern int player_data_count;
extern MobData mob_data[MAX_MOBS];
#endif
