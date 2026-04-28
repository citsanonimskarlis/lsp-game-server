// Bomberman Serveris - main.c
// Kompilē: gcc -o server main.c -Wall -Wextra
// Palaiž: ./server 25565 map.txt

#define STATE_IMPL
#include "state.h"

#include <signal.h>

#include <errno.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SERVER_ID        "BombermanSrv/1.0"
#define MAX_BOMBS        64
#define TICK_MS          (1000 / TICKS_PER_SECOND)   /* 50 ms */
#define NO_ROUND_LIMIT   0                            /* 0 = bez laika ierobežojuma */

/* ── Globālie mainīgie ─────────────────────────────────────── */
static int       client_fds[MAX_PLAYERS];
static GameState game;
static bomb_t    bombs[MAX_BOMBS];
static int       bomb_count = 0;

/* Spēlētāju statistika (atiestatās katram raundam) */
static uint16_t stat_kills[MAX_PLAYERS];
static uint16_t stat_blocks[MAX_PLAYERS];

/* Raunda taimeris */
static uint32_t  round_max_ticks = NO_ROUND_LIMIT;

/* ── Kartes konfigurācija ──────────────────────────────────── */
static uint16_t cfg_speed      = 4;
static uint8_t  cfg_radius     = 1;
static uint16_t cfg_bomb_timer = 60;
static uint16_t cfg_dmg_time   = 5;
static uint16_t cfg_round_ticks = NO_ROUND_LIMIT;  /* no kartes faila */

static uint16_t spawn_row[MAX_PLAYERS];
static uint16_t spawn_col[MAX_PLAYERS];
static int      spawn_found[MAX_PLAYERS];

static const char *g_map_file = NULL;

static void handle_disconnect(int fd);

static int safe_send(int fd, const uint8_t *buf, int len);

/* ── Laika palīgs ──────────────────────────────────────────── */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ══════════════════════════════════════════════════════════════
   PALĪGFUNKCIJAS
   ══════════════════════════════════════════════════════════════ */


static void send_to(int fd, const uint8_t *buf, int len) {
    safe_send(fd, buf, len);
}

static int find_player_by_fd(int fd) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (client_fds[i] == fd) return i;
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (client_fds[i] == -1) return i;
    return -1;
}

static int count_connected(void) {
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (client_fds[i] >= 0) n++;
    return n;
}

/* ══════════════════════════════════════════════════════════════
   ZIŅU SŪTĪŠANA — apvienotas palīgfunkcijas
   ══════════════════════════════════════════════════════════════ */
static void broadcast(const uint8_t *buf, int len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] >= 0) {
            if (safe_send(client_fds[i], buf, len) < 0) {
                client_fds[i] = -1;
            }
        }
    }
}
/* 4-baitu broadcast: [type, 255, 254, data] */
static void broadcast_simple(uint8_t type, uint8_t data) {
    uint8_t buf[4] = {type, 255, 254, data};
    broadcast(buf, 4);
}

/* 6-baitu broadcast ar šūnas koordināti: [type, 255, 254, data, cell_hi, cell_lo] */
static void broadcast_cell_event(uint8_t type, uint8_t data,
                                  uint16_t row, uint16_t col) {
    uint8_t buf[6];
    buf[0] = type;
    buf[1] = 255;
    buf[2] = 254;
    buf[3] = data;
    uint16_t cell = htons(make_cell_index(row, col, game.map_width));
    memcpy(&buf[4], &cell, 2);
    broadcast(buf, 6);
}

/* 5-baitu broadcast blokam: [type, 255, 254, cell_hi, cell_lo] */
static void broadcast_block_destroyed(uint16_t row, uint16_t col) {
    uint8_t buf[5];
    buf[0] = MSG_BLOCK_DESTROYED;
    buf[1] = 255;
    buf[2] = 254;
    uint16_t cell = htons(make_cell_index(row, col, game.map_width));
    memcpy(&buf[3], &cell, 2);
    broadcast(buf, 5);
}

/* ERROR ziņa uz vienu klientu */
static void send_error(int fd, const char *msg) {
    uint16_t msglen = (uint16_t)strlen(msg);
    uint8_t buf[3 + 2 + 256];
    buf[0] = MSG_ERROR;
    buf[1] = 255;
    buf[2] = 255;
    uint16_t net_len = htons(msglen);
    memcpy(&buf[3], &net_len, 2);
    memcpy(&buf[5], msg, msglen);
    send_to(fd, buf, 5 + msglen);
}

/* Kartes sūtīšana */
static void send_map(int fd) {
    int map_size = game.map_height * game.map_width;
    uint8_t *buf = malloc(5 + (size_t)map_size);
    if (!buf) return;
    buf[0] = MSG_MAP;
    buf[1] = 255;
    buf[2] = 254;
    buf[3] = game.map_height;
    buf[4] = game.map_width;
    memcpy(&buf[5], game.map, (size_t)map_size);
    send_to(fd, buf, 5 + map_size);
    free(buf);
}

static void send_welcome(int fd, int new_id) {
    uint8_t buf[3 + 20 + 1 + 1 + MAX_PLAYERS * 32];
    memset(buf, 0, sizeof(buf));

    buf[0] = MSG_WELCOME;
    buf[1] = (uint8_t)new_id;
    buf[2] = (uint8_t)new_id;
    strncpy((char *)&buf[3], SERVER_ID, 20);
    buf[23] = (uint8_t)game.status;

    int count  = 0;
    int offset = 25;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] >= 0 && i != new_id) {
            buf[offset]     = (uint8_t)i;
            buf[offset + 1] = (uint8_t)game.players[i].ready;
            strncpy((char *)&buf[offset + 2], game.players[i].name, 30);
            offset += 32;
            count++;
        }
    }
    buf[24] = (uint8_t)count;
    send_to(fd, buf, 25 + count * 32);
}

/* ══════════════════════════════════════════════════════════════
   KARTES IELĀDE
   ══════════════════════════════════════════════════════════════ */

static bool load_map(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) { perror("Nevar atvērt kartes failu"); return false; }

    int h, w, speed, dmg_time, radius, bomb_time, round_time;

    /* Kartes pirmā rinda: h w speed dmg_time radius bomb_time [round_ticks] */
    int n = fscanf(f, "%d %d %d %d %d %d",
                   &h, &w, &speed, &dmg_time, &radius, &bomb_time);
    if (n < 6) {
        fprintf(stderr, "Nekorekts kartes fails\n");
        fclose(f); return false;
    }
    /* Neobligāts 7. lauks — raunda maksimālais ilgums ticks */
    if (fscanf(f, " %d", &round_time) == 1 && round_time > 0)
        cfg_round_ticks = (uint16_t)round_time;
    else
        cfg_round_ticks = NO_ROUND_LIMIT;

    cfg_speed      = (uint16_t)speed;
    cfg_radius     = (uint8_t)radius;
    cfg_bomb_timer = (uint16_t)bomb_time;
    cfg_dmg_time   = (uint16_t)dmg_time;

    game.map_height = (uint8_t)h;
    game.map_width  = (uint8_t)w;

    memset(spawn_found, 0, sizeof(spawn_found));

    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            char cell;
            if (fscanf(f, " %c", &cell) != 1) break;
            int idx = r * w + c;
            if (cell >= '1' && cell <= '8') {
                int p = cell - '1';
                spawn_row[p]   = (uint16_t)r;
                spawn_col[p]   = (uint16_t)c;
                spawn_found[p] = 1;
                game.map[idx]  = '.';
            } else {
                game.map[idx] = (uint8_t)cell;
            }
        }
    }
    fclose(f);
    printf("Karte ielādēta: %dx%d, ātrums=%d, rādiuss=%d, taimeris=%d ticks, raunds=%d ticks\n",
           h, w, speed, radius, bomb_time, cfg_round_ticks);
    return true;
}

/* ══════════════════════════════════════════════════════════════
   SPĒLES SĀKŠANA / BEIGŠANA
   ══════════════════════════════════════════════════════════════ */

static void check_win_condition(void);

static void start_game(void) {
    load_map(g_map_file);
    bomb_count = 0;

    memset(stat_kills,  0, sizeof(stat_kills));
    memset(stat_blocks, 0, sizeof(stat_blocks));
    memset(game.bonuses_collected, 0, sizeof(game.bonuses_collected));

    /* Raunda taimeris */
    if (cfg_round_ticks > 0)
        round_max_ticks = cfg_round_ticks;
    else
        round_max_ticks = NO_ROUND_LIMIT;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] >= 0) {
            game.players[i].alive            = true;
            game.players[i].speed            = cfg_speed;
            game.players[i].bomb_radius      = cfg_radius;
            game.players[i].bomb_timer_ticks = cfg_bomb_timer;
            game.players[i].bomb_count       = 1;
            if (spawn_found[i]) {
                game.players[i].row = spawn_row[i];
                game.players[i].col = spawn_col[i];
            }
        } else {
            game.players[i].alive = false;
        }
    }

    game.status = GAME_RUNNING;
    broadcast_simple(MSG_SET_STATUS, GAME_RUNNING);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] >= 0) {
            send_map(client_fds[i]);
            broadcast_cell_event(MSG_MOVED, (uint8_t)i,
                                 game.players[i].row,
                                 game.players[i].col);
        }
    }
    printf("Spēle sākta! Spēlētāji: %d\n", count_connected());
}

static void end_game(int winner_id) {
    game.status = GAME_END;

    /* Statistikas izvadīšana serverī */
    printf("=== RAUNDA STATISTIKA ===\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] >= 0)
            printf("  [%d] %s  kills=%u  blocks=%u  bonuses=%u\n",
                   i, game.players[i].name,
                   stat_kills[i], stat_blocks[i],
                   game.bonuses_collected[i]);
    }

    if (winner_id >= 0) {
        game.winner_id = (uint8_t)winner_id;
        printf("Uzvarētājs: spēlētājs %d (%s)\n",
               winner_id, game.players[winner_id].name);
        broadcast_simple(MSG_WINNER, (uint8_t)winner_id);
    } else {
        game.winner_id = 255;
        printf("Spēle beidzās — neizšķirts.\n");
    }
    broadcast_simple(MSG_SET_STATUS, GAME_END);

    for (int i = 0; i < MAX_PLAYERS; i++)
        game.players[i].ready = false;

    round_max_ticks = NO_ROUND_LIMIT;
}

static void check_win_condition(void) {
    if (game.status != GAME_RUNNING) return;

    int alive_count = 0;
    int winner_id = -1;
    int connected_count = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] >= 0) {
            connected_count++;
            if (game.players[i].alive) {
                alive_count++;
                winner_id = i;
            }
        }
    }
    if (alive_count == 0) {
        end_game(-1); // Neizšķirts, ja visi beigti
    } else if (alive_count == 1 && connected_count > 1) {
        end_game(winner_id); // Viens uzvarētājs
    }
}


/* ══════════════════════════════════════════════════════════════
   KUSTĪBA
   ══════════════════════════════════════════════════════════════ */

static bool cell_passable(int r, int c, int moving_player_id) {
    if (r < 0 || r >= game.map_height || c < 0 || c >= game.map_width)
        return false;
    uint8_t tile = game.map[r * game.map_width + c];
    if (tile == 'H' || tile == 'S' || tile == 'B')
        return false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == moving_player_id) continue;
        if (!game.players[i].alive) continue;
        if (game.players[i].row == (uint16_t)r &&
            game.players[i].col == (uint16_t)c)
            return false;
    }
    return true;
}

static void handle_move(int fd, uint8_t dir_byte) {
    int id = find_player_by_fd(fd);
    if (id < 0) return;
    if (!game.players[id].alive) {
        send_error(fd, "Player is dead");
        return;
    }

    int r = game.players[id].row;
    int c = game.players[id].col;

    switch (dir_byte) {
        case 'U': r--; break;
        case 'D': r++; break;
        case 'L': c--; break;
        case 'R': c++; break;
        default:
            send_error(fd, "Invalid direction");
            return;
    }

    if (!cell_passable(r, c, id)) {
        send_error(fd, "Cell not passable");
        return;
    }

    game.players[id].row = (uint16_t)r;
    game.players[id].col = (uint16_t)c;
    broadcast_cell_event(MSG_MOVED, (uint8_t)id, (uint16_t)r, (uint16_t)c);

    /* Bonusa savākšana */
    int idx = r * game.map_width + c;
    uint8_t tile = game.map[idx];
    if (tile == 'A' || tile == 'R' || tile == 'T' || tile == 'N') {
        broadcast_cell_event(MSG_BONUS_RETRIEVED, (uint8_t)id,
                             (uint16_t)r, (uint16_t)c);
        game.bonuses_collected[id]++;
        if      (tile == 'A') game.players[id].speed++;
        else if (tile == 'R') game.players[id].bomb_radius++;
        else if (tile == 'T') game.players[id].bomb_timer_ticks += 10;
        else if (tile == 'N') game.players[id].bomb_count++;
        game.map[idx] = '.';
    }
}

/* ══════════════════════════════════════════════════════════════
   BUMBAS UN SPRĀDZIENI
   ══════════════════════════════════════════════════════════════ */

static void detonate_bomb(int bi) {
    bomb_t *b = &bombs[bi];
    int cr = b->row;
    int cc = b->col;
    int radius = b->radius;
    uint8_t owner = b->owner_id;

    /* 1. Noņem bumbu no kartes un sāk sprādzienu */
    game.map[cr * game.map_width + cc] = '.';
    broadcast_cell_event(MSG_EXPLOSION_START, (uint8_t)radius, (uint16_t)cr, (uint16_t)cc);

    int dr[] = {-1, 1, 0, 0};
    int dc[] = {0, 0, -1, 1};

    /* 2. Atzīmē sprādziena centru */
    game.map[cr * game.map_width + cc] = '*';

    /* 3. Izskaitļo sprādziena starus četros virzienos */
    for (int dir = 0; dir < 4; dir++) {
        for (int step = 1; step <= radius; step++) {
            int r = cr + dr[dir] * step;
            int c = cc + dc[dir] * step;

            if (r < 0 || r >= game.map_height || c < 0 || c >= game.map_width) break;

            int idx = r * game.map_width + c;
            uint8_t tile = game.map[idx];

            if (tile == 'H') break; /* Hard wall apstādina staru */

            if (tile == 'S') {
                /* Soft wall tiek iznīcināts (atzīmējam ar 'X' priekš EXPLOSION_END) */
                game.map[idx] = 'X';
                stat_blocks[owner]++;
                break; /* Stars tālāk neiet */
            }

            if (tile == 'B') {
                /* Ķēdes reakcija: atrodam otru bumbu un detonējam to nākamajā tick */
                for (int k = 0; k < bomb_count; k++) {
                    if (bombs[k].row == (uint16_t)r && bombs[k].col == (uint16_t)c) {
                        bombs[k].timer_ticks = 0; 
                        break;
                    }
                }
                game.map[idx] = '*';
                continue; /* Stars iet cauri bumbai */
            }

            game.map[idx] = '*';
        }
    }

    /* 4. Spēlētāju nāves pārbaude */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        /* Pārbaudām tikai tos, kas ir pieslēgušies un vēl dzīvi */
        if (client_fds[i] < 0 || !game.players[i].alive) continue;

        int pr = game.players[i].row;
        int pc = game.players[i].col;
        uint8_t tile = game.map[pr * game.map_width + pc];

        /* Ja spēlētājs atrodas sprādziena zonā ('*' vai 'X') */
        if (tile == '*' || tile == 'X') {
            game.players[i].alive = false;
            broadcast_simple(MSG_DEATH, (uint8_t)i);
            
            /* Ja bumbas īpašnieks nogalina kādu citu, pieskaitām kill */
            if (owner != (uint8_t)i) {
                stat_kills[owner]++;
            }
            printf("SERVERIS: Spēlētājs %d (%s) miris sprādzienā.\n", i, game.players[i].name);
        }
    }

    /* --- KRITISKAIS LABOJUMS --- */
    /* Pārbaudām uzvaras nosacījumus uzreiz pēc tam, kad kāds ir miris */
    check_win_condition();
    /* --------------------------- */

    /* Iestatām bumbas atlikušo laiku sprādziena vizualizācijai */
    b->timer_ticks = cfg_dmg_time;
    b->lives = 0; /* Bumba vairs nav aktīva */
}

static void tick_bombs(void) {
    /* 1. kārta: aktīvās bumbas (tās, kas vēl tikai gaida sprādzienu) */
    for (int i = 0; i < bomb_count; i++) {
        if (bombs[i].lives == 1) {
            if (bombs[i].timer_ticks > 0)
                bombs[i].timer_ticks--;

            if (bombs[i].timer_ticks == 0) {
                detonate_bomb(i);
                // Pēc detonācijas bombs[i].lives kļūst par 0
            }
        }
        if (game.status != GAME_RUNNING) return;
    }

    /* 2. kārta: sprāgušās bumbas — gaida, kad pazudīs sarkanās liesmas (*) */
    int i = 0;
    while (i < bomb_count) {
        if (bombs[i].lives == 0) {
            if (bombs[i].timer_ticks > 0) {
                bombs[i].timer_ticks--;
            }

            if (bombs[i].timer_ticks == 0) {
                int cr = bombs[i].row;
                int cc = bombs[i].col;
                int radius = bombs[i].radius;
                int map_w = game.map_width;
                int map_h = game.map_height;

                // 1. Sūtam klientam ziņu, ka sprādziens beidzies
                broadcast_cell_event(MSG_EXPLOSION_END, (uint8_t)radius, (uint16_t)cr, (uint16_t)cc);

                // 2. Notīrām sprādziena centru
                int center_idx = cr * map_w + cc;
                if (center_idx >= 0 && center_idx < (map_w * map_h)) {
                    if (game.map[center_idx] == '*') {
                        game.map[center_idx] = '.';
                    }
                }

                int dr[] = {-1, 1, 0, 0};
                int dc[] = {0, 0, -1, 1};

                // 3. Notīrām sprādziena starus
                for (int dir = 0; dir < 4; dir++) {
                    for (int step = 1; step <= radius; step++) {
                        int r = cr + dr[dir] * step;
                        int c = cc + dc[dir] * step;

                        // Pārbaude, vai neizejam no kartes
                        if (r < 0 || r >= map_h || c < 0 || c >= map_w) break;

                        int idx = r * map_w + c;
                        uint8_t tile = game.map[idx];

                        if (tile == 'H') break; // Akmens sienu stars nepāriet

                        if (tile == '*') {
                            game.map[idx] = '.';
                        } else if (tile == 'X') {
                            // Šeit ir mīkstais bloks, kas tikko izjuka
                            game.map[idx] = '.';
                            broadcast_block_destroyed((uint16_t)r, (uint16_t)c);

                            // Bonusa loģika (droša)
                            if (rand() % 3 == 0) {
                                int btype = 1 + (rand() % 4);
                                char bchars[] = {'.', 'A', 'R', 'T', 'N'};
                                if (btype >= 1 && btype <= 4) {
                                    game.map[idx] = (uint8_t)bchars[btype];
                                    broadcast_cell_event(MSG_BONUS_AVAILABLE, (uint8_t)btype, (uint16_t)r, (uint16_t)c);
                                }
                            }
                            break; // Stars apstājas pie bloka
                        } else if (tile == 'B') {
                             // Ja stars atduras pret citu bumbu, tas iet cauri (ķēdes reakcija notiek detonate_bomb)
                             continue;
                        } else {
                            // Ja tur ir kaut kas cits (piem. bonuss), stars iet cauri vai apstājas
                            // Atkarībā no tavas spēles loģikas.
                        }
                    }
                }

                // 4. IZDZĒŠAM bumbas objektu no saraksta
                // Pārliekam pēdējo bumbu uz šo vietu un samazinām skaitu
                bombs[i] = bombs[bomb_count - 1];
                bomb_count--;
                
                // Svarīgi: nepalielinām 'i', jo tagad bombs[i] ir jauna bumba!
                continue; 
            }
        }
        i++;
    }

    if (game.status != GAME_RUNNING) return;

    /* Raunda laika kontrole */
    if (round_max_ticks > 0) {
        if (round_max_ticks == 1) {
            printf("LAIKS BEIDZIES!\n");
            end_game(-1);
            return;
        }
        round_max_ticks--;
    }
    check_win_condition();
}

static void handle_bomb(int fd) {
    int id = find_player_by_fd(fd);
    if (id < 0) return;
    if (!game.players[id].alive) {
        send_error(fd, "Player is dead");
        return;
    }
    if (game.status != GAME_RUNNING) return;

    int used = 0;
    for (int i = 0; i < bomb_count; i++)
        if (bombs[i].owner_id == (uint8_t)id && bombs[i].lives == 1)
            used++;
    if (used >= game.players[id].bomb_count) {
        send_error(fd, "No bombs available");
        return;
    }

    uint16_t r = game.players[id].row;
    uint16_t c = game.players[id].col;
    int idx = r * game.map_width + c;

    if (game.map[idx] == 'B') {
        send_error(fd, "Cell already has a bomb");
        return;
    }
    if (bomb_count >= MAX_BOMBS) {
        send_error(fd, "Max bombs reached");
        return;
    }

    bomb_t *b = &bombs[bomb_count++];
    b->owner_id    = (uint8_t)id;
    b->row         = r;
    b->col         = c;
    b->radius      = game.players[id].bomb_radius;
    b->timer_ticks = game.players[id].bomb_timer_ticks;
    b->lives       = 1;
    game.map[idx]  = 'B';
    broadcast_cell_event(MSG_BOMB, (uint8_t)id, r, c);
}

/* ══════════════════════════════════════════════════════════════
   PAKEŠU APSTRĀDE
   ══════════════════════════════════════════════════════════════ */
static int safe_send(int fd, const uint8_t *buf, int len) {
    if (fd < 0) return -1;

    int n = send(fd, buf, (size_t)len, MSG_NOSIGNAL);

    if (n <= 0) {
        if (errno == EPIPE || errno == ECONNRESET) {
            handle_disconnect(fd);
        }
        return -1;
    }
    return n;
}

static void handle_hello(int new_fd, uint8_t *buf) {
    if (game.status != GAME_LOBBY) {
        uint8_t disc[3] = {MSG_DISCONNECT, 255, 0};
        send_to(new_fd, disc, 3);
        close(new_fd);
        return;
    }

    int slot = find_free_slot();
    if (slot == -1) {
        uint8_t disc[3] = {MSG_DISCONNECT, 255, 0};
        send_to(new_fd, disc, 3);
        close(new_fd);
        return;
    }

    client_fds[slot] = new_fd;
    game.players[slot].is_connected = true;
    game.players[slot].alive        = false;
    game.players[slot].ready        = false;
    /* Spēlētāja vārds sākas pie buf[23]: 3 header + 20 client_name */
    strncpy(game.players[slot].name, (char *)&buf[23], MAX_PLAYER_NAME - 1);
    game.players[slot].name[MAX_PLAYER_NAME - 1] = '\0';

    printf("Spēlētājs %d (%s) pieslēdzās.\n",
           slot, game.players[slot].name);

    /* Paziņo pārējiem */
    uint8_t hello_fwd[53];
    memcpy(hello_fwd, buf, 53);
    hello_fwd[1] = (uint8_t)slot;
    hello_fwd[2] = 254;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (client_fds[i] >= 0 && i != slot)
            send_to(client_fds[i], hello_fwd, 53);

    send_welcome(new_fd, slot);
    send_map(new_fd);
}

static void handle_set_ready(int fd) {
    int id = find_player_by_fd(fd);
    if (id < 0) return;

    game.players[id].ready = true;

    uint8_t buf[3] = {MSG_SET_READY, (uint8_t)id, 254};
    broadcast(buf, 3);

    int connected   = count_connected();
    int ready_count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (client_fds[i] >= 0 && game.players[i].ready) ready_count++;

    printf("Gatavi: %d/%d\n", ready_count, connected);

    if (ready_count == connected && connected >= 2)
        start_game();
}

static void handle_set_status(int fd, uint8_t *buf) {
    (void)fd;
    uint8_t new_status = buf[3];
    if (game.status == GAME_END && new_status == GAME_LOBBY) {
        game.status = GAME_LOBBY;
        for (int i = 0; i < MAX_PLAYERS; i++)
            game.players[i].ready = false;
        broadcast_simple(MSG_SET_STATUS, GAME_LOBBY);
        printf("Atgriežas lobijā.\n");
    }
}

static void handle_packet(int fd, uint8_t *buf, int len) {
    int pos = 0;
    while (pos < len) {
        uint8_t type = buf[pos];
        
        // Pārbaudām, vai mums ir pietiekami daudz datu šai ziņai
        switch (type) {
            case MSG_MOVE_ATTEMPT:
                if (pos + 4 <= len && game.status == GAME_RUNNING) {
                    handle_move(fd, buf[pos + 3]);
                }
                pos += 4; // Kustības ziņa ir 4 baiti
                break;

            case MSG_BOMB_ATTEMPT:
                // Klienta bumbas ziņa parasti ir 5 baiti
                if (pos + 5 <= len && game.status == GAME_RUNNING) {
                    handle_bomb(fd);
                }
                pos += 5; 
                break;

            case MSG_SET_READY:
                handle_set_ready(fd);
                pos += 3; // READY ziņa ir 3 baiti
                break;

            case MSG_SET_STATUS:
                if (pos + 4 <= len) handle_set_status(fd, &buf[pos]);
                pos += 4;
                break;

            case MSG_PING:
                if (pos + 3 <= len) {
                    uint8_t pong[3] = {MSG_PONG, buf[pos+1], buf[pos+2]};
                    send_to(fd, pong, 3);
                }
                pos += 3;
                break;

            case MSG_HELLO:
                pos += 53; // HELLO ir liela pakete
                break;

            default:
                // Ja nezinām ziņu, labāk apstāties, lai nesabojātu atmiņu
                pos = len; 
                break;
        }
    }
}

static void handle_disconnect(int fd) {
    int id = find_player_by_fd(fd);
    if (id >= 0) {
        printf("Spēlētājs %d (%s) atvienojās.\n",
               id, game.players[id].name);

        client_fds[id]                = -1;
        game.players[id].is_connected = false;
        game.players[id].alive        = false;
        game.players[id].ready        = false;

        uint8_t buf[3] = {MSG_LEAVE, (uint8_t)id, 254};
        broadcast(buf, 3);

        if (game.status == GAME_RUNNING)
            check_win_condition();

        close(fd);
    }
}



/* ══════════════════════════════════════════════════════════════
   GALVENĀ FUNKCIJA
   ══════════════════════════════════════════════════════════════ */

#include <signal.h> // Nepieciešams signālu apstrādei

int main(int argc, char *argv[]) {
    
    // 1. Ignorējam SIGPIPE, lai serveris nenobruktu, sūtot datus atvienotam klientam
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    if (argc < 3) {
        printf("Lietošana: %s <ports> <karte.txt>\n", argv[0]);
        return 1;
    }

    srand((unsigned)time(NULL));

    int port   = atoi(argv[1]);
    g_map_file = argv[2];

    init_game_state(&game);
    for (int i = 0; i < MAX_PLAYERS; i++)
        client_fds[i] = -1;

    if (!load_map(g_map_file))
        return 1;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family         = AF_INET;
    addr.sin_port           = htons((uint16_t)port);
    addr.sin_addr.s_addr    = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(server_fd, MAX_PLAYERS);
    printf("Serveris klausās uz porta %d...\n", port);

    long next_tick = now_ms() + TICK_MS;

    while (1) {
        long now  = now_ms();
        long wait = next_tick - now;
        if (wait < 0) wait = 0;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        int max_fd = server_fd;

        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (client_fds[i] >= 0) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd) max_fd = client_fds[i];
            }
        }

        struct timeval tv;
        tv.tv_sec  = wait / 1000;
        tv.tv_usec = (wait % 1000) * 1000;

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (ready > 0 && FD_ISSET(server_fd, &read_fds)) {
            int new_fd = accept(server_fd, NULL, NULL);
            if (new_fd >= 0) {
                uint8_t buf[53];
                int n = (int)recv(new_fd, buf, 53, MSG_WAITALL);
                if (n == 53 && buf[0] == MSG_HELLO) {
                    handle_hello(new_fd, buf);
                } else {
                    close(new_fd);
                }
            }
        }

        if (ready > 0) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (client_fds[i] >= 0 &&
                    FD_ISSET(client_fds[i], &read_fds)) {
                    uint8_t buf[256];
                    int n = (int)recv(client_fds[i], buf, sizeof(buf), 0);
                    if (n <= 0) {
                        handle_disconnect(client_fds[i]);
                    } else {
                        handle_packet(client_fds[i], buf, n);
                    }
                }
            }
        }

        now = now_ms();
        if (now >= next_tick) {
            if (game.status == GAME_RUNNING)
                tick_bombs();
            next_tick += TICK_MS;
            if (now_ms() > next_tick)
                next_tick = now_ms() + TICK_MS;
        }
    }

    return 0;
}
