#include "vfs.h"
#include <ctype.h>
#include <errno.h>
#include <string.h>

static int find_shared_index(struct vm_args* va, const char* path) {
    if (!va || !va->shared_files)
        return -1;
    for (int i = 0; i < va->shared_count; ++i) {
        if (strcmp(va->shared_files[i].path, path) == 0)
            return i;
    }
    return -1;
}

static const char* get_local_copy_if_any(struct vm_args* va, const char* orig_path) {
    if (!va)
        return NULL;
    for (int i = 0; i < va->cow_count; ++i) {
        if (va->cow_map[i].active && strcmp(va->cow_map[i].orig_path, orig_path) == 0)
            return va->cow_map[i].local_path;
    }
    return NULL;
}

static int ensure_local_copy(struct vm_args* va, const char* orig_path, char* out_local_path, size_t out_len) {
    if (!va || !orig_path || !out_local_path)
        return -1;

    // return if already exists
    const char* existing = get_local_copy_if_any(va, orig_path);
    if (existing) {
        strncpy(out_local_path, existing, out_len - 1);
        out_local_path[out_len - 1] = '\0';
        return 0;
    }

    // create local copy in /tmp
    const char* base = strrchr(orig_path, '/');
    base = base ? base + 1 : orig_path;
    char local[MAX_PATH_LEN];
    snprintf(local, sizeof(local), "/tmp/hv_vm%d_%s_%d", va->vm_id, base, getpid());

    FILE* src = fopen(orig_path, "rb");
    if (!src)
        return -1;
    FILE* dst = fopen(local, "wb");
    if (!dst) {
        fclose(src);
        return -1;
    }

    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, r, dst) != r) {
            fclose(src);
            fclose(dst);
            return -1;
        }
    }
    fclose(src);
    fclose(dst);

    if (va->cow_count >= MAX_COW_ENTRIES)
        return -1;
    int idx = va->cow_count++;
    va->cow_map[idx].active = 1;
    strncpy(va->cow_map[idx].orig_path, orig_path, MAX_PATH_LEN - 1);
    va->cow_map[idx].orig_path[MAX_PATH_LEN - 1] = '\0';
    strncpy(va->cow_map[idx].local_path, local, MAX_PATH_LEN - 1);
    va->cow_map[idx].local_path[MAX_PATH_LEN - 1] = '\0';

    strncpy(out_local_path, local, out_len - 1);
    out_local_path[out_len - 1] = '\0';
    return 0;
}

static int validate_filename(const char* name) {
    if (!name || !name[0])
        return 0;
    if (!isalpha((unsigned char)name[0]))
        return 0;
    for (const char* p = name; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalpha(c) || isdigit(c) || c == '.' || c == '_')
            continue;
        return 0;
    }
    return 1;
}

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
    if (guest_flags == 9) {
        return O_WRONLY | O_APPEND;
    }
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
    /*
    PROTOCOL:
    outb(cmd=V_FILE_OPEN),
    outb(char) for each character of the filename,
    outb(0)  (terminator, marks end of filename),
    outb(flags)  (O_RD=1, O_WR=2, O_RDWR=4, O_CREATE=8, O_APPEND=9)
    result: 1 inb call returning the guest fd (signed 8-bit) or -1 on error
    */

    // collectin' flags
    if (!ps->waiting_for_flag) {
        if (value == 0) {
            ps->path_buf[ps->path_len] = '\0';
            ps->waiting_for_flag = 1;
        } else {
            if (ps->path_len < MAX_PATH_LEN - 1) {
                ps->path_buf[ps->path_len++] = (char)value;
            }
        }
        return;
    }
    // flags collected check filename
    int guest_flags = (int)value;
    if (!validate_filename(ps->path_buf)) {
        va->last_result = -1;
        ps->current_cmd = 0;
        ps->path_len = 0;
        ps->waiting_for_flag = 0;
        ps->path_buf[0] = '\0';
        return;
    }
    int host_flags = guest_flags_to_host(guest_flags);

    int shared_idx = find_shared_index(va, ps->path_buf);
    if (shared_idx < 0 && va->shared_files && va->shared_count > 0) {
        for (int i = 0; i < va->shared_count; ++i) {
            const char* shared_path = va->shared_files[i].path;
            const char* base = strrchr(shared_path, '/');
            base = base ? base + 1 : shared_path;
            if (strcmp(base, ps->path_buf) == 0) {
                shared_idx = i;
                break;
            }
        }
    }
    int host_fd = -1;
    if (shared_idx >= 0) {
        // deljeni fajl
        int wants_write = ((host_flags & O_WRONLY) || (host_flags & O_RDWR) || (guest_flags & O_CREAT));
        if (wants_write) {
            const char* existing_local = get_local_copy_if_any(va, va->shared_files[shared_idx].path);
            char local_path[MAX_PATH_LEN];
            if (existing_local) {
                host_fd = open(existing_local, host_flags | O_CREAT, 0644);
            } else {
                if (ensure_local_copy(va, va->shared_files[shared_idx].path, local_path, sizeof(local_path)) == 0) {
                    host_fd = open(local_path, host_flags | O_CREAT, 0644);
                } else {
                    host_fd = -1;
                }
            }
        } else {
            host_fd = open(va->shared_files[shared_idx].path, host_flags);
        }
    } else {
        // obican fajl
        if (host_flags & O_CREAT) {
            host_fd = open(ps->path_buf, host_flags, 0644);
        } else {
            host_fd = open(ps->path_buf, host_flags);
        }
    }
    int guest_fd = (host_fd >= 0) ? allocate_guest_fd(va, host_fd) : -1;
    va->last_result = guest_fd;

    ps->current_cmd = 0;
    ps->path_len = 0;
    ps->waiting_for_flag = 0;
    ps->path_buf[0] = '\0';
}

void process_close(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    /*
    PROTOCOL:
    outb(cmd=V_FILE_CLOSE),
    outb(fd)
    result: 1 inb call returning 0 on success or -1 on error
    */
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
    /*
    PROTOCOL:
    outb(cmd=V_FILE_READ),
    outb(fd),
    outb(count)  (number of bytes requested)
    result: 1 inb call returning n (number of bytes actually read, signed 8-bit,
            or -1 on error), followed by n inb calls returning the read bytes
    */
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
            va->read_pending = 1;
        }
        // reset
        ps->tmp_fd = -1;
        ps->tmp_count = 0;
        ps->current_cmd = 0;
    }
}

void process_write(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    /*
    PROTOCOL:
    outb(cmd=V_FILE_WRITE),
    outb(fd),
    outb(count)  (number of bytes to write),
    outb(byte) for each of the count bytes to write
    result: 1 inb call returning n (number of bytes actually written, signed 8-bit,
            or -1 on error)
    */
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
            // reset
            ps->w_fd = -1;
            ps->w_count = 0;
            ps->w_pos = 0;
            ps->current_cmd = 0;
        }
    }
}

void process_lseek(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    /*
    PROTOCOL:
    outb(cmd=V_FILE_LSEEK),
    outb(fd),
    outb(offset_byte0), outb(offset_byte1), outb(offset_byte2), outb(offset_byte3),
    outb(whence)  (1 = SEEK_SET, 2 = SEEK_END, 3 = SEEK_CUR)
    result: 4 inb calls (LSB first) returning the 32-bit signed offset
            or 0xFFFFFFFF on error
    */
    if (ps->l_fd == -1) {
        ps->l_fd = (int)value;
    } else if (ps->l_offset_pos < 4) {
        ps->l_offset_acc |= ((uint32_t)value) << (8 * ps->l_offset_pos);
        ps->l_offset_pos++;
    } else {
        ps->l_whence = (int)value;
        int host_fd = map_fd(va, ps->l_fd);
        off_t res = (off_t)-1;
        if (host_fd >= 0) {
            int wh = SEEK_SET;
            if (ps->l_whence == 2)
                wh = SEEK_END;
            else if (ps->l_whence == 3)
                wh = SEEK_CUR;
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
        // reset
        ps->l_fd = -1;
        ps->l_offset_acc = 0;
        ps->l_offset_pos = 0;
        ps->l_whence = 0;
        ps->current_cmd = 0;
    }
}
