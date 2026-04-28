// state.h
#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MAX_PLAYERS 8
#define TICKS_PER_SECOND 20
#define MAX_PLAYER_NAME 30

#define MAX_SERVER_NAME 20

#define MAX_GRID_SIZE 255

typedef enum { GAME_LOBBY = 0, GAME_RUNNING = 1, GAME_END = 2 } game_status_t;

typedef enum {
  DIR_UP = 0,
  DIR_DOWN = 1,
  DIR_LEFT = 2,
  DIR_RIGHT = 3
} direction_t;

typedef enum {
  BONUS_NONE = 0,
  BONUS_SPEED = 1,
  BONUS_RADIUS = 2,
  BONUS_TIMER = 3
} bonus_type_t;

typedef enum {
  MSG_HELLO = 0,
  MSG_WELCOME = 1,
  MSG_DISCONNECT = 2,
  MSG_PING = 3,
  MSG_PONG = 4,
  MSG_LEAVE = 5,
  MSG_ERROR = 6,
  MSG_MAP = 7,
  MSG_SET_READY = 10,
  MSG_SET_STATUS = 20,
  MSG_WINNER = 23,
  MSG_MOVE_ATTEMPT = 30,
  MSG_BOMB_ATTEMPT = 31,
  MSG_MOVED = 40,
  MSG_BOMB = 41,
  MSG_EXPLOSION_START = 42,
  MSG_EXPLOSION_END = 43,
  MSG_DEATH = 44,
  MSG_BONUS_AVAILABLE = 45,
  MSG_BONUS_RETRIEVED = 46,
  MSG_BLOCK_DESTROYED = 47,
  MSG_SYNC_BOARD = 100,
  MSG_SYNC_REQUEST = 101
} msg_type_t;

typedef struct {
  uint8_t id;
  char name[MAX_PLAYER_NAME];
  uint16_t row;
  uint16_t col;
  bool alive;
  bool ready;
  uint8_t bomb_count;
  uint8_t bomb_radius;
  uint16_t bomb_timer_ticks;
  uint16_t speed;

  bool is_connected;
} player_t;

typedef struct {
  uint8_t lives; // 0 nozīme beigts
  uint8_t owner_id;
  uint16_t row;
  uint16_t col;
  uint8_t radius;
  uint16_t timer_ticks;
} bomb_t;

static inline uint16_t make_cell_index(uint16_t row, uint16_t col,
                                       uint16_t cols) {
  return row * cols + col;
}

typedef struct {
  game_status_t status; // LOBBY, RUNNING, END
  uint8_t map_width;
  uint8_t map_height;
  uint8_t map[MAX_GRID_SIZE * MAX_GRID_SIZE]; // The grid data
  player_t players[MAX_PLAYERS];              // Max 8 players (page 11)
  char server_name[MAX_SERVER_NAME];

  uint8_t my_player_id;

  bool is_initialized; // after we recieve welcome packet it is set to
                       // true when we disconnect to false
  //
  uint8_t winner_id;

  uint8_t spectate_target; // player ID to follow camera; 255 = follow self

  // Per-player statistics
  uint16_t bonuses_collected[MAX_PLAYERS];

} GameState;

void init_game_state(GameState *gama);

#ifdef STATE_IMPL
void init_game_state(GameState *game) {
  game->status = GAME_LOBBY;
  game->map_width = 0;
  game->map_height = 0;
  memset(game->map, 0, sizeof(game->map));

  for (int i = 0; i < MAX_PLAYERS; i++) {
    player_t *player = &game->players[i];

    player->alive = false;
    player->col = 0;
    player->row = 0;
    player->id = (uint8_t)i;
    player->is_connected = false;
    memset(player->name, '\0', sizeof(player->name));
    player->bomb_count = 1;         // spec: starts with 1 bomb
    player->bomb_radius = 1;        // spec: radius 1
    player->bomb_timer_ticks = 60;  // spec: 3 seconds = 60 ticks
    player->ready = false;
    player->speed = 4;              // spec: 4 cells/second
  }
  memset(game->server_name, '\0', sizeof(game->server_name));
  game->my_player_id = 255;
  game->is_initialized = false;
  game->winner_id = 255;
  game->spectate_target = 255;
  memset(game->bonuses_collected, 0, sizeof(game->bonuses_collected));
}
#endif

#endif
