#if !defined(PROTOCOL_H)
#define PROTOCOL_H
#include <stdint.h>
#include <string.h>
struct parser_state {
    int current_cmd;

    char path_buf[128];
    int path_len;
    int waiting_for_flag;

    int tmp_fd;
    int tmp_count;

    int w_fd;
    int w_count;
    int w_pos;
    char w_buf[512];

    int l_fd;
    uint32_t l_offset_acc;
    int l_offset_pos;
    int l_whence;

    int result32_pending;
    uint32_t result32_val;
    int result32_pos;
};

void parser_init(struct parser_state* ps);
void parser_set_value(struct parser_state* ps, uint8_t value);
#endif // PROTOCOL_H
