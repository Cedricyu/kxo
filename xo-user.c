#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

#define READBUFFER_SIZE 12
#define BOARD_SIZE 16

// ====== Coroutine 宏與結構 ======
struct cr {
    int state;
    bool finished;
};
#define cr_begin(cr)       \
    switch ((cr)->state) { \
    case 0:
#define cr_end(cr) }
#define cr_yield(cr)            \
    do {                        \
        (cr)->state = __LINE__; \
        return;                 \
    case __LINE__:;             \
    } while (0)
#define cr_exit(cr)            \
    do {                       \
        (cr)->finished = true; \
        return;                \
    } while (0)


char move_sequence[128] = "";
void add_move_to_sequence(char col, int row)
{
    char move[8];
    snprintf(move, sizeof(move), "%c%d", col, row);
    if (strlen(move_sequence) > 0)
        strcat(move_sequence, " -> ");
    strcat(move_sequence, move);
}

unsigned char prev_bitmap[READBUFFER_SIZE] = {0};

static inline bool is_first_move(const unsigned char *curr_bitmap)
{
    int non_zero_cells = 0;
    for (int i = 0; i < BOARD_SIZE; ++i) {
        int byte_index = (i * 2) / 8;
        int bit_offset = (i * 2) % 8;
        u_int8_t val = (curr_bitmap[byte_index] >> bit_offset) & 0x3;
        if (val != 0)
            non_zero_cells++;
    }
    return non_zero_cells == 1;
}

static int game_cnt = 0;
void detect_move(unsigned char *curr_bitmap)
{
    for (int i = 0; i < BOARD_SIZE; ++i) {
        int byte_index = (i * 2) / 8;
        int bit_offset = (i * 2) % 8;
        u_int8_t prev_val = (prev_bitmap[byte_index] >> bit_offset) & 0x3;
        u_int8_t curr_val = (curr_bitmap[byte_index] >> bit_offset) & 0x3;

        if (is_first_move(curr_bitmap)) {
            add_move_to_sequence('G', game_cnt++);
            return;
        }
        if (prev_val != curr_val) {
            // 偵測到第 i 個格子的狀態改變
            char cols[] = "ABCD";
            int row = i / 4;
            int col = i % 4;
            add_move_to_sequence(cols[col], row + 1);
            printf("Detected move: %c%d\n", cols[col], row + 1);
            break;  // 假設每次只有一個移動
        }
    }
    memcpy(prev_bitmap, curr_bitmap, READBUFFER_SIZE);
}

void draw_board_user(unsigned char *bitmap)
{
    for (int i = 0; i < 16; ++i) {
        int byte_index = (i * 2) / 8;
        int bit_offset = (i * 2) % 8;
        u_int8_t val = (bitmap[byte_index] >> bit_offset) & 0x3;

        char c = ' ';
        if (val == 1)
            c = 'O';
        else if (val == 2)
            c = 'X';

        printf("%c", c);
        if (i % 4 != 3)
            printf("|");
        else
            printf("\n");
    }
}

// ====== 狀態檢查 ======
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

// ====== Raw mode 處理 ======
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

// ====== 全域旗標 ======
static bool read_attr, end_attr;

// ====== 鍵盤處理邏輯 ======
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
            printf("Moves: %s\n", move_sequence);
            break;
        }
    }
    close(attr_fd);
}

// ====== keyboard_loop coroutine ======
void keyboard_loop(struct cr *cr)
{
    cr_begin(cr);
    while (!cr->finished) {
        // 等待 stdin 可讀
        while (true) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(STDIN_FILENO, &set);
            struct timeval tv = {0, 0};  // 非阻塞
            int ret = select(STDIN_FILENO + 1, &set, NULL, NULL, &tv);
            if (ret > 0 && FD_ISSET(STDIN_FILENO, &set))
                break;
            cr_yield(cr);
        }
        listen_keyboard_handler();
        if (end_attr)
            cr_exit(cr);
        cr_yield(cr);
    }
    cr_end(cr);
}

// ====== display_loop coroutine ======
void display_loop(struct cr *cr, int device_fd, char *display_buf)
{
    cr_begin(cr);
    while (!cr->finished) {
        // 等待 device_fd 可讀，且 read_attr 為 true
        while (true) {
            if (!read_attr) {
                cr_yield(cr);
                continue;
            }
            fd_set set;
            FD_ZERO(&set);
            FD_SET(device_fd, &set);
            struct timeval tv = {0, 0};  // 非阻塞
            int ret = select(device_fd + 1, &set, NULL, NULL, &tv);
            if (ret > 0 && FD_ISSET(device_fd, &set))
                break;
            cr_yield(cr);
        }
        printf("\033[H\033[J");  // 清螢幕
        ssize_t n = read(device_fd, display_buf, READBUFFER_SIZE);
        if (n <= 0) {
            cr_yield(cr);
            continue;
        }
        unsigned char *bitmap = display_buf;
        u_int32_t ai1_ms = ((u_int32_t *) display_buf)[1];
        u_int32_t ai2_ms = ((u_int32_t *) display_buf)[2];
        printf("AI1 Avg Time: %u ms\n", ai1_ms);
        printf("AI2 Avg Time: %u ms\n", ai2_ms);
        detect_move(display_buf);
        draw_board_user(display_buf);
        cr_yield(cr);
    }
    cr_end(cr);
}

// ====== main & scheduler ======
int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char display_buf[READBUFFER_SIZE];
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);

    read_attr = true;
    end_attr = false;

    struct cr keyboard_cr = {0, false};
    struct cr display_cr = {0, false};

    // coroutine scheduler
    while (!end_attr) {
        keyboard_loop(&keyboard_cr);
        display_loop(&display_cr, device_fd, display_buf);
        usleep(1000);  // 輕量 sleep，避免忙等
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
