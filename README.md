# KVM Hypervisor

A minimal hypervisor in C built directly on the Linux KVM API. It runs up to 10 x86-64 guests at the same time, one host thread per VM.

Course project for Computer Architecture and Organization 2, School of Electrical Engineering, University of Belgrade.

## Features

- **VM setup:** creates the VM and vCPU through `/dev/kvm` ioctls and puts the guest straight into 64-bit long mode with its own page tables. Pages are 4 KB or 2 MB, and guest memory is 2, 4 or 8 MB.
- **Exit handling:** handles `KVM_EXIT_IO`, `HLT`, `SHUTDOWN`, `IRQ_WINDOW_OPEN`, `FAIL_ENTRY` and `INTERNAL_ERROR`.
- **Serial console:** guests write to port `0xE9`, and output from all VMs is synchronized.
- **Virtual file system over port I/O:** guests call `open`, `close`, `read`, `write` and `lseek` through a byte-level protocol on port `0x278`. Host files passed with `-f` are shared between guests copy-on-write, so each VM writes to its own private copy.
- **Inter-VM shared buffer:** VM 0 is the writer and the other VMs are readers. Readers are notified through an injected interrupt, and the host side is synchronized with pthreads.
- **Freestanding guest:** built with GCC (`-ffreestanding`) and a custom linker script.

## Layout

```
host/   hypervisor: VM setup, exit handling, VFS, shared buffer, console
guest/  freestanding x86-64 guest code and linker script
test/   guest test programs (console, file system, shared buffer)
```

## Build and run

You need Linux with KVM enabled (`/dev/kvm`), plus GCC and Make.

```bash
make
./host/build/hypervisor -m 4 -p 2 -g guest/build/guest.img guest/build/guest.img
```

| Option | Meaning |
|---|---|
| `-m <2\|4\|8>` | guest memory in MB |
| `-p <4\|2>` | page size: 4 KB or 2 MB |
| `-g <img> [img ...]` | guest images to run, one VM each (max 10) |
| `-f <path>` | host file shared with guests (copy-on-write), repeatable |
