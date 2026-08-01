#include "protocol.h"

void parser_init(struct parser_state* ps) {
    if (!ps)
        return;
    memset(ps, 0, sizeof(*ps));

    ps->current_cmd = 0;

    ps->path_len = 0;
    ps->waiting_for_flag = 0;

    ps->tmp_fd = -1;
    ps->tmp_count = 0;

    ps->w_fd = -1;
    ps->w_count = 0;
    ps->w_pos = 0;

    ps->l_fd = -1;
    ps->l_offset_acc = 0;
    ps->l_offset_pos = 0;
    ps->l_whence = 0;

    ps->result32_pending = 0;
    ps->result32_val = 0;
    ps->result32_pos = 0;
}

void parser_set_value(struct parser_state* ps, uint8_t value) {
    ps->current_cmd = (int)value;
    ps->path_len = 0;
    ps->waiting_for_flag = 0;

    ps->tmp_fd = -1;
    ps->tmp_count = 0;

    ps->w_fd = -1;
    ps->w_count = 0;
    ps->w_pos = 0;
    
    ps->l_fd = -1;
    ps->l_offset_acc = 0;
    ps->l_offset_pos = 0;
    ps->l_whence = 0;
}