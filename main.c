#define STATE_IMPL
#include "state.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SERVER_NAME "BombermanServer"
#define MAX_BOMBS 64

static int client_fds[MAX_PLAYERS];
static GameState game;
static bomb_t bombs[MAX_BOMBS];
static int bomb_count = 0;

void broadcast(uint8_t *buf, int len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] >= 0) {
            send(client_fds[i], buf, len, 0);
        }
    }
}

int find_player_by_fd(int fd) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] == fd) return i;
    }
    return -1;
}

int find_free_slot() {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] == -1) return i;
    }
    return -1;
}

int count_connected() {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] >= 0) count++;
    }
    return count;
}

int count_alive() {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game.players[i].alive) count++;
    }
    return count;
}

void send_welcome(int fd, int new_id) {
    uint8_t buf[3 + 20 + 1 + 1 + MAX_PLAYERS * 32];
    memset(buf, 0, sizeof(buf));
    buf[0] = MSG_WELCOME;
    buf[1] = (uint8_t)new_id;
    buf[2] = (uint8_t)new_id;
    strncpy((char *)&buf[3], SERVER_NAME, 20);
    buf[23] = (uint8_t)game.status;
    int count = 0;
    int offset = 25;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (client_fds[i] >= 0 && i != new_id) {
            buf[offset] = (uint8_t)i;
            buf[offset + 1] = game.players[i].ready;
            strncpy((char *)&buf[offset + 2], game.players[i].name, 30);
            offset += 32;
            count++;
        }
    }
    buf[24] = (uint8_t)count;
    send(fd, buf, 25 + count * 32, 0);
}

void handle_hello(int new_fd, uint8_t *buf) {
    int slot = find_free_slot();
    if (slot == -1) {
        printf("Nav brīvu vietu!\n");
        close(new_fd);
    } else {
        client_fds[slot] = new_fd;
        strncpy(game.players[slot].name, (char *)&buf[23], 30);
        game.players[slot].is_connected = true;
        send_welcome(new_fd, slot);
    }
}

void handle_packet(int fd, uint8_t *buf, int len) {
    (void)len;
    switch (buf[0]) {
        case MSG_PING: {
            uint8_t pong[3] = {MSG_PONG, buf[1], 255};
            send(fd, pong, 3, 0);
            break;
        }
    }
}

void handle_disconnect(int fd) {
    int id = find_player_by_fd(fd);
    if (id == -1){
        return;
    }
    game.players[id].is_connected = false;
    game.players[id].alive = false;
    client_fds[id] = -1;
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Lietošana: %s <ports>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    init_game_state(&game);
    for (int i = 0; i < MAX_PLAYERS; i++) client_fds[i] = -1;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, MAX_PLAYERS);

    while (1) {
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
        struct timeval tv = {0, 50000};
        select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (FD_ISSET(server_fd, &read_fds)) {
            int new_fd = accept(server_fd, NULL, NULL);
            uint8_t buf[53];
            if (recv(new_fd, buf, 53, MSG_WAITALL) == 53 && buf[0] == MSG_HELLO) {
                handle_hello(new_fd, buf);
            }
        }
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (client_fds[i] >= 0 && FD_ISSET(client_fds[i], &read_fds)) {
                uint8_t buf[1024];
                int n = recv(client_fds[i], buf, sizeof(buf), 0);
                if (n <= 0){
                    handle_disconnect(client_fds[i]);
                }else{
                    handle_packet(client_fds[i], buf, n);
                } 
            }
        }
    }
    return 0;
}
