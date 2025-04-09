#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

#define MAX_GAMES 10

typedef struct {
    int pos;
    char player;
} move_t;

/* Structure to store a complete game */
typedef struct {
    move_t moves[N_GRIDS];
    int move_count;
} game_t;

/* Array to store history of multiple games */
static game_t games[MAX_GAMES];
static int game_count = 0;

/* tracking current game */
static move_t current_moves[N_GRIDS];
static int move_count = 0;
static char prev_board[N_GRIDS];

static char empty_board[N_GRIDS];

static void pos_to_coord(int pos, char *coord)
{
    coord[0] = 'A' + GET_COL(pos);
    coord[1] = '1' + GET_ROW(pos);
    coord[2] = '\0';
}

static int count_pieces(const char *board)
{
    int count = 0;
    for (int i = 0; i < N_GRIDS; i++) {
        if (board[i] != ' ')
            count++;
    }
    return count;
}

static void save_current_game(void)
{
    if (game_count < MAX_GAMES) {
        games[game_count].move_count = move_count;
        memcpy(games[game_count].moves, current_moves,
               sizeof(move_t) * move_count);
        game_count++;
    } else {
        /* shift the games stroage, always keep latest MAX_GAMES data */
        for (int i = 0; i < MAX_GAMES - 1; i++) {
            games[i] = games[i + 1];
        }

        games[MAX_GAMES - 1].move_count = move_count;
        memcpy(games[MAX_GAMES - 1].moves, current_moves,
               sizeof(move_t) * move_count);
    }
}

static void track_moves(const char *board)
{
    int curr_pieces = count_pieces(board);
    int prev_pieces = count_pieces(prev_board);

    if (curr_pieces < prev_pieces) {
        if (move_count > 0) {
            save_current_game();
            move_count = 0;
            for (int i = 0; i < N_GRIDS; i++) {
                if (board[i] != ' ') {
                    if (move_count < N_GRIDS) {
                        current_moves[move_count].pos = i;
                        current_moves[move_count].player = board[i];
                        move_count++;
                    }
                }
            }
        }
    }

    /* Track new moves */
    for (int i = 0; i < N_GRIDS; i++) {
        if (board[i] != ' ' && prev_board[i] == ' ') {
            if (move_count < N_GRIDS) {
                current_moves[move_count].pos = i;
                current_moves[move_count].player = board[i];
                move_count++;
            }
        }
    }

    /* Update previous board state */
    memcpy(prev_board, board, N_GRIDS);
}
static void display_move_history(void)
{
    if (move_count > 0) {
        save_current_game();
    }

    printf("\nGame History (%d games):\n", game_count);
    if (game_count == 0) {
        printf("No games were recorded.\n");
        return;
    }

    for (int g = 0; g < game_count; g++) {
        printf("Game %d: ", g + 1);
        printf("Moves: ");

        for (int i = 0; i < games[g].move_count; i++) {
            char coord[3];
            pos_to_coord(games[g].moves[i].pos, coord);
            printf("%c%s", games[g].moves[i].player, coord);
            if (i < games[g].move_count - 1) {
                printf(" -> ");
            }
        }
        printf("\n");
    }
}


/* Draw the board on the terminal */
static int draw_board(const char *table)
{
    printf("\n\n");
    for (int i = 0; i < BOARD_SIZE; i++) {
        for (int j = 0; j < BOARD_SIZE; j++) {
            printf(" %c ", table[i * BOARD_SIZE + j]);
            if (j < BOARD_SIZE - 1)
                printf("|");
        }
        printf("\n");
        if (i < BOARD_SIZE - 1) {
            for (int j = 0; j < BOARD_SIZE * 4 - 1; j++) {
                printf("-");
            }
            printf("\n");
        }
    }
    printf("\n");
}

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~IXON;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool read_attr, end_attr;

static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("Stopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("Stopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}

int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char board_data[N_GRIDS];

    memset(empty_board, ' ', N_GRIDS);
    memset(prev_board, ' ', N_GRIDS);

    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;

    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);

        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {
            printf("Error with select system call\n");
            exit(1);
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (FD_ISSET(device_fd, &readset)) {
            FD_CLR(device_fd, &readset);
            printf("\033[H\033[J"); /* ASCII escape code to clear the screen */

            read(device_fd, board_data, N_GRIDS);
            track_moves(board_data);
            if (read_attr)
                draw_board(board_data);
        }
    }

    display_move_history();

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
