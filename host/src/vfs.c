#include "vfs.h"

int allocate_guest_fd(struct vm_args* va, int host_fd) {
    if (va->next_guest_fd >= MAX_FDS)
        return -1;
    int guest_fd = va->next_guest_fd++;
    va->guest_fd_to_host_fd[guest_fd] = host_fd;
    return guest_fd;
}

int map_fd(struct vm_args* va, int guest_fd) {
    if (guest_fd < 0 || guest_fd >= MAX_FDS)
        return -1;
    return va->guest_fd_to_host_fd[guest_fd];
}

int guest_flags_to_host(int guest_flags) {
    int host_flags = 0;
    if (guest_flags & 4)
        host_flags |= O_RDWR;
    else if (guest_flags & 2)
        host_flags |= O_WRONLY;
    else
        host_flags |= O_RDONLY;
    if (guest_flags & 8)
        host_flags |= O_CREAT;
    return host_flags;
}

void process_open(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    if (!ps->waiting_for_flag) {
        if (value != 0) {
            if (ps->path_len < (int)sizeof(ps->path_buf) - 1) {
                ps->path_buf[ps->path_len++] = (char)value;
            }
        } else {
            ps->path_buf[ps->path_len] = '\0';
            ps->waiting_for_flag = 1;
        }
    } else {
        int guest_flags = (int)value;
        int host_flags = guest_flags_to_host(guest_flags);
        int host_fd;
        if (host_flags & O_CREAT)
            host_fd = open(ps->path_buf, host_flags, 0644);
        else
            host_fd = open(ps->path_buf, host_flags);
        int guest_fd = (host_fd >= 0) ? allocate_guest_fd(va, host_fd) : -1;
        va->last_result = guest_fd;
        /* reset parser state */
        ps->current_cmd = 0;
        ps->path_len = 0;
        ps->waiting_for_flag = 0;
    }
}

void process_close(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    int guest_fd = (int)value;
    int host_fd = map_fd(va, guest_fd);
    int res = -1;
    if (host_fd >= 0)
        res = close(host_fd);
    if (guest_fd >= 0 && guest_fd < MAX_FDS)
        va->guest_fd_to_host_fd[guest_fd] = -1;
    va->last_result = (res == 0) ? 0 : -1;
    ps->current_cmd = 0;
}

void process_read(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    if (ps->tmp_fd == -1) {
        ps->tmp_fd = (int)value;
    } else {
        ps->tmp_count = (int)value;
        int host_fd = map_fd(va, ps->tmp_fd);
        if (host_fd < 0) {
            va->last_result = -1;
            va->read_buffer_len = 0;
            va->read_buffer_pos = 0;
            va->read_pending = 0;
        } else {
            int toread = ps->tmp_count;
            if (toread > (int)sizeof(va->read_buffer))
                toread = (int)sizeof(va->read_buffer);
            int n = read(host_fd, va->read_buffer, toread);
            va->read_buffer_len = (n > 0) ? n : 0;
            va->read_buffer_pos = 0;
            va->last_result = n;
            va->read_pending = 1; /* next IN returns n, then data */
        }
        /* reset */
        ps->tmp_fd = -1;
        ps->tmp_count = 0;
        ps->current_cmd = 0;
    }
}

void process_write(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    if (ps->w_fd == -1) {
        ps->w_fd = (int)value;
    } else if (ps->w_count == 0) {
        ps->w_count = (int)value;
        if (ps->w_count < 0)
            ps->w_count = 0;
        if (ps->w_count > (int)sizeof(ps->w_buf))
            ps->w_count = (int)sizeof(ps->w_buf);
        ps->w_pos = 0;
    } else {
        if (ps->w_pos < (int)sizeof(ps->w_buf)) {
            ps->w_buf[ps->w_pos++] = (char)value;
        }
        if (ps->w_pos >= ps->w_count) {
            int host_fd = map_fd(va, ps->w_fd);
            if (host_fd < 0) {
                va->last_result = -1;
            } else {
                int n = write(host_fd, ps->w_buf, ps->w_count);
                va->last_result = n;
            }
            /* reset */
            ps->w_fd = -1;
            ps->w_count = 0;
            ps->w_pos = 0;
            ps->current_cmd = 0;
        }
    }
}

void process_lseek(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    /* protokol:
       outb(cmd=5), outb(fd),
       outb(offset_byte0), outb(offset_byte1), outb(offset_byte2), outb(offset_byte3),
       outb(whence)  (1 = SEEK_SET, 2 = SEEK_END)
       rezultat: 4 inb poziva (LSB first) koji vraćaju 32-bit signed offset ili 0xFFFFFFFF na grešku
    */
    if (ps->l_fd == -1) {
        ps->l_fd = (int)value;
    } else if (ps->l_offset_pos < 4) {
        ps->l_offset_acc |= ((uint32_t)value) << (8 * ps->l_offset_pos);
        ps->l_offset_pos++;
    } else {
        /* poslednji bajt: whence */
        ps->l_whence = (int)value;
        int host_fd = map_fd(va, ps->l_fd);
        off_t res = (off_t)-1;
        if (host_fd >= 0) {
            int wh = SEEK_SET;
            if (ps->l_whence == 2)
                wh = SEEK_END;
            /* interpretiramo l_offset_acc kao signed 32-bit */
            int32_t soff = (int32_t)ps->l_offset_acc;
            res = lseek(host_fd, (off_t)soff, wh);
        }
        if (res == (off_t)-1) {
            ps->result32_val = 0xFFFFFFFFu;
        } else {
            ps->result32_val = (uint32_t)res;
        }
        ps->result32_pending = 1;
        ps->result32_pos = 0;
        /* reset lseek parser state */
        ps->l_fd = -1;
        ps->l_offset_acc = 0;
        ps->l_offset_pos = 0;
        ps->l_whence = 0;
        ps->current_cmd = 0;
    }
}
