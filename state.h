// state.h
#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_PLAYERS 8
#define TICKS_PER_SECOND 20
#define MAX_NAME_LEN 15

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
  char name[MAX_NAME_LEN + 1];
  uint16_t row;
  uint16_t col;
  bool alive;
  bool ready;
  uint8_t bomb_count;
  uint8_t bomb_radius;
  uint16_t bomb_timer_ticks;
  uint16_t speed;
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
} GameState;

#endif
