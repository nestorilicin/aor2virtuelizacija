#ifndef CONSOLE_H
#define CONSOLE_H

#include "vm.h"
#include <stddef.h>

int console_init(void);

void console_destroy(void);

void console_write_char(struct vm_args* va, char c);

void console_flush(struct vm_args* va);

#endif /* CONSOLE_H */
