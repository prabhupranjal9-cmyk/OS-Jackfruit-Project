# Lab: Memory Management

> **Platform:** Ubuntu 22.04
> 
> **Install:** `sudo apt update && sudo apt install -y gcc build-essential linux-tools-common linux-tools-$(uname -r) sysstat strace`

---

## Section 0 - Get Your Bearings

Before we start poking at memory, let's figure out what we're working with. Record these numbers - everything you see later only makes sense relative to your machine's baseline.

```bash
# First, set your prompt to your SRN (last 3 digits only)
# Example: if your SRN is PES1UG24CS042, enter 042
export PS1="042\$ "
```

```bash
# How much RAM and swap do you have?
free -m

# What's the page size? (spoiler: it's 4096 on basically everything)
getconf PAGESIZE

# How many physical page frames does your system have?
awk '/MemTotal/ {print $2 / 4}' /proc/meminfo
```

**Screenshot 0:** Output of `free -m` showing your VM's RAM and swap.

> **Q0.1:** If your VM has 2048 MB of RAM and the page size is 4096 bytes, how many physical page frames exist? Show your arithmetic.

---

## Section 1 - The Virtual Address Space

Every process thinks it owns one big flat stretch of memory, all to itself. In reality, the OS carves up the virtual address space into separate regions - code, data, heap, stack - and only bothers mapping physical RAM to them when something actually needs it.

### The Textbook Picture

You've probably seen this diagram in slides. Now you're going to check whether your actual machine agrees with it:

```
  High Address (0x7FFF...)
  ┌─────────────────────────┐
  │        Stack            │  ← grows DOWNWARD (toward lower addresses)
  │        ↓ ↓ ↓            │
  │                         │
  │   (unmapped gap)        │
  │                         │
  │        ↑ ↑ ↑            │
  │        Heap             │  ← grows UPWARD (toward higher addresses)
  ├─────────────────────────┤
  │   BSS  (uninit globals) │  ← zeroed by OS at load time
  ├─────────────────────────┤
  │   Data (init globals)   │  ← global_init = 42 lives here
  ├─────────────────────────┤
  │   Rodata (constants)    │  ← string literals, const data
  ├─────────────────────────┤
  │   Text (code)           │  ← your compiled instructions
  └─────────────────────────┘
  Low Address (0x400000 for non-PIE)
```

### Experiment

```bash
# Start by looking at your shell's own memory map
cat /proc/self/maps
```

Take a moment to read the columns: `address-range  perms  offset  dev  inode  pathname`. You'll see these a lot.

```bash
# Now let's write a program that prints where each region lives
cat << 'EOF' > layout.c
#include <stdio.h>
#include <stdlib.h>

int global_init = 42;          // .data - initialized global
int global_uninit;             // .bss  - uninitialized global (zeroed)
const char *rodata = "hello";  // .rodata - read-only constant

int main(void) {
    int stack_var = 7;
    int *heap_var = malloc(64);

    printf("%-20s %p\n", "Text  (main):",    (void *)main);
    printf("%-20s %p\n", "Rodata:",          (void *)rodata);
    printf("%-20s %p\n", "Data  (init):",    (void *)&global_init);
    printf("%-20s %p\n", "BSS   (uninit):",  (void *)&global_uninit);
    printf("%-20s %p\n", "Heap  (malloc):",  (void *)heap_var);
    printf("%-20s %p\n", "Stack (local):",   (void *)&stack_var);

    // Print the gap between heap and stack
    printf("\nHeap-to-Stack gap: %lu MB\n",
           ((unsigned long)&stack_var - (unsigned long)heap_var) / (1024*1024));

    printf("\n--- /proc/self/maps (filtered) ---\n");
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
        "cat /proc/%d/maps | grep -E 'heap|stack|layout'", getpid());
    system(cmd);

    free(heap_var);
    return 0;
}
EOF
gcc -o layout layout.c -no-pie
./layout
```

You should see six addresses. Text, Rodata, Data, and BSS will cluster together down low (around `0x400000`). The heap sits just above them. The stack is way up in high memory (around `0x7FFF...`). The gap between the heap and stack is absurdly large - we're talking thousands of GB of virtual space that doesn't map to anything.

> **Q1.1:** List the six addresses from lowest to highest. Does the ordering match the diagram? Verify: Text < Rodata < Data < BSS < Heap ≪ Stack.
>
> **Q1.2:** In the filtered `/proc/self/maps` output, what permissions does `[heap]` have? Why `rw-p` instead of `rwxp`? What could go wrong if heap memory were executable?
>
> **Q1.3:** Compile again *with* PIE: `gcc -o layout_pie layout.c` and run it twice. Do the addresses move between runs? Compare with the non-PIE version. Look up ASLR (Address Space Layout Randomization) - why is this a security feature?

**Screenshot 1:** Full output of `./layout` (the non-PIE version).

---

## Section 2 - Demand Paging: malloc Doesn't Actually Allocate Memory

Interesting thing: calling `malloc(N)` does not give you N bytes of physical RAM. All it does is reserve virtual address space.

Physical frames only show up one page at a time, the first time you actually *read or write* to a page. That first touch triggers a page fault, and the kernel responds by finding a free physical frame and mapping it in. This lazy approach is called demand paging, and it's why your system can hand out way more virtual memory than it physically has.

Two numbers to keep an eye on:

- **VmSize** - total virtual address space the process has reserved. Think of it as the "promise."
- **VmRSS** (Resident Set Size) - how much physical RAM the process is actually using right now. This is the truth.

```c
// demand.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MB (1024 * 1024)

static void show_rss(const char *label) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "grep -E 'VmSize|VmRSS|VmSwap' /proc/%d/status", getpid());
    printf("\n[%s]\n", label);
    fflush(stdout);
    system(buf);
}

int main(void) {
    show_rss("1. before malloc");

    // Reserve 256 MB of VIRTUAL memory
    char *big = malloc(256 * MB);
    if (!big) { perror("malloc"); return 1; }

    show_rss("2. after malloc, before touch - virtual only!");

    // Touch first 64 MB → 64 MB / 4 KB = 16384 page faults
    memset(big, 'A', 64 * MB);
    show_rss("3. after touching 64 MB");

    // Touch all 256 MB → remaining 192 MB causes more faults
    memset(big, 'B', 256 * MB);
    show_rss("4. after touching all 256 MB");

    free(big);
    return 0;
}
```

```bash
gcc -O0 -o demand demand.c
./demand
```

Watch VmSize and VmRSS carefully at each stage. VmSize jumps right after `malloc` - but VmRSS? It barely moves. RSS only catches up once you start writing to those pages. That's demand paging in action.

> **Q2.1:** After `malloc` but before any touch, how much did VmSize go up? How much did VmRSS go up? Explain the difference in one sentence.
>
> **Q2.2:** Run `perf stat -e page-faults ./demand`. How many page faults did you get? Does it roughly match `256 MB / 4 KB = 65536`? If it's a bit higher, think about what else causes faults - the program's code, libc, the stack, etc.
>
> **Q2.3:** Your classmate says "my program uses 256 MB" right after calling `malloc(256 * MB)`. Correct them in one sentence using the terms VmSize, RSS, and demand paging.

**Screenshot 2:** All four `[label]` blocks showing VmSize/VmRSS at each stage.

---

## Section 3 - Page Tables: How Virtual Addresses Become Physical

When your program touches an address, the CPU doesn't just go straight to RAM. It walks through a page table - a multi-level lookup structure - to translate the virtual address into a physical one. On x86-64 Linux, it's 4 levels deep:

```
  Virtual Address (48 bits used)
  ┌────────┬────────┬────────┬────────┬──────────────┐
  │ PGD(9) │ PUD(9) │ PMD(9) │ PTE(9) │ Offset(12)   │
  └───┬────┴───┬────┴───┬────┴───┬────┴──────────────┘
      │        │        │        │
      ▼        ▼        ▼        ▼
   Level 4 → Level 3 → Level 2 → Level 1 → Physical Frame + Offset = Physical Address
```

Each level uses 9 bits, so each table has 512 entries. The bottom-level entry (the PTE) holds the important stuff:

- **Present bit** - is this page currently in physical RAM, or not?
- **PFN** (Page Frame Number) - which physical frame holds the data
- **Permission bits** - read, write, execute, user vs kernel

Linux lets you peek at this through `/proc/pid/pagemap`. Let's do that:

```c
// pagemap.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static void query_pagemap(const char *label, void *vaddr) {
    uint64_t data;
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned long vpn = (unsigned long)vaddr / page_size;

    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) { perror("open pagemap"); return; }

    // Each pagemap entry is 8 bytes; seek to the entry for this VPN
    if (lseek(fd, vpn * 8, SEEK_SET) == (off_t)-1) {
        perror("lseek"); close(fd); return;
    }
    if (read(fd, &data, 8) != 8) {
        perror("read"); close(fd); return;
    }
    close(fd);

    int present  = (data >> 63) & 1;   // bit 63: page in RAM?
    int swapped  = (data >> 62) & 1;   // bit 62: page on swap?
    uint64_t pfn = data & 0x7FFFFFFFFFFFFF;  // bits 0-54: frame number

    printf("  %-14s VA=%p  present=%d  swapped=%d", label, vaddr, present, swapped);
    if (present)
        printf("  PFN=0x%lx  →  PA=0x%lx",
               (unsigned long)pfn, (unsigned long)(pfn * page_size));
    else
        printf("  (no physical frame assigned)");
    printf("\n");
}

int main(void) {
    char *p = malloc(4096 * 4);  // allocate 4 pages

    printf("=== Before touching any page ===\n");
    query_pagemap("page[0]:", p);
    query_pagemap("page[1]:", p + 4096);
    query_pagemap("page[2]:", p + 8192);

    // Touch only pages 0 and 1
    p[0] = 'X';
    p[4096] = 'Y';

    printf("\n=== After touching pages 0 and 1 ===\n");
    query_pagemap("page[0]:", p);
    query_pagemap("page[1]:", p + 4096);
    query_pagemap("page[2]:", p + 8192);  // never touched

    free(p);
    return 0;
}
```

```bash
gcc -O0 -o pagemap pagemap.c
# pagemap needs root privileges (or CAP_SYS_ADMIN)
sudo ./pagemap
```

Before you touch anything, all three pages should show `present=0` - no physical frame. After writing to pages 0 and 1, those two flip to `present=1` and get assigned real PFNs. Page 2, which you never touched, stays unmapped. This is exactly the demand paging behavior from Section 2, but now you're seeing it at the page table level.

> **Q3.1:** What's the `present` bit for each page before and after touching? Which pages get a physical frame? Connect this to what you saw in Section 2.
>
> **Q3.2:** Page 2 was allocated by `malloc` but never written to. Is it present? Does it have a PFN? What does that tell you about the page table entry for pages that are allocated but never touched?
>
> **Q3.3:** Run it twice. Are the PFNs the same? Why would they differ? (The kernel just grabs whatever frame happens to be free at that moment.)

**Screenshot 3:** Output of `sudo ./pagemap` showing before/after touch.

---

## Section 4 - Copy-on-Write: How fork() Doesn't Actually Copy Anything

When a process calls `fork()`, the child gets what looks like a full copy of the parent's entire address space. But the kernel is lazy about it - in a good way. It doesn't copy a single page. Instead, both parent and child point to the exact same physical frames, and the kernel marks all those pages as read-only:

```
  BEFORE ANY WRITE (pages are shared):
  ┌──────────┐     ┌──────────────┐
  │  Parent  │────▶│ Physical     │
  │  PTE: R--│     │ Frame #42    │
  └──────────┘     │ data: "PPP"  │
  ┌──────────┐     │              │
  │  Child   │────▶│              │
  │  PTE: R--│     └──────────────┘
  └──────────┘

  AFTER CHILD WRITES (copy made on demand):
  ┌──────────┐     ┌──────────────┐
  │  Parent  │────▶│ Frame #42    │
  │  PTE: RW │     │ data: "PPP"  │  ← unchanged, now private to parent
  └──────────┘     └──────────────┘
  ┌──────────┐     ┌──────────────┐
  │  Child   │────▶│ Frame #99    │
  │  PTE: RW │     │ data: "CCC"  │  ← fresh copy, private to child
  └──────────┘     └──────────────┘
```

The trick: as long as both processes only *read*, they share the same memory and everything is fine. The moment either one tries to *write*, the hardware traps (because the page is marked read-only), the kernel makes a private copy of just that one page, and then allows the write. That's a COW (Copy-on-Write) fault.

```c
// cow.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MB (1024 * 1024)
#define ALLOC_MB 128

static long get_minor_faults(void) {
    // Field 10 in /proc/self/stat is minflt (minor page faults)
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return -1;

    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);

    // Skip past the comm field "(name)" which may contain spaces
    char *p = strrchr(line, ')');
    if (!p) return -1;
    p += 2;  // skip ") "

    // Parse: state, ppid, pgrp, session, tty, tpgid, flags, minflt
    long minflt;
    char state;
    int dummy;
    unsigned long flags;
    sscanf(p, "%c %d %d %d %d %d %lu %ld",
           &state, &dummy, &dummy, &dummy, &dummy, &dummy, &flags, &minflt);
    return minflt;
}

int main(void) {
    char *region = malloc(ALLOC_MB * MB);
    memset(region, 'P', ALLOC_MB * MB);  // parent touches all pages

    printf("Parent: allocated and touched %d MB (%d pages)\n",
           ALLOC_MB, ALLOC_MB * 256);

    pid_t pid = fork();

    if (pid == 0) {
        // ---- CHILD ----
        long faults_start = get_minor_faults();

        // READ-ONLY pass: pages are shared, shouldn't trigger COW
        volatile char sink = 0;
        for (int i = 0; i < ALLOC_MB * MB; i += 4096)
            sink += region[i];
        long faults_after_read = get_minor_faults();

        printf("\nChild READ-ONLY scan:\n");
        printf("  Page faults: %ld  (should be ~0, pages are shared)\n",
               faults_after_read - faults_start);

        // WRITE pass: every page triggers a COW fault
        long faults_before_write = get_minor_faults();
        memset(region, 'C', ALLOC_MB * MB);
        long faults_after_write = get_minor_faults();

        printf("\nChild WRITE pass:\n");
        printf("  Page faults: %ld\n",
               faults_after_write - faults_before_write);
        printf("  Expected:    %d  (one COW fault per page)\n",
               ALLOC_MB * 256);

        free(region);
        _exit(0);
    }

    // ---- PARENT ----
    wait(NULL);
    free(region);
    return 0;
}
```

```bash
gcc -O0 -o cow cow.c
./cow
```

The read-only scan should cause almost zero faults - the child is reading the parent's pages through shared mappings, no copying needed. The write pass should cause roughly 32768 faults (128 MB ÷ 4 KB = 32768 pages), one COW fault for each page the child dirties.

> **Q4.1:** How many page faults during the read-only scan? Why nearly zero? Explain using the COW diagram above.
>
> **Q4.2:** How many faults during the write pass? How close is it to the expected 32768? If it's off by a small number, what else might have caused a few extra faults?
>
> **Q4.3:** If `fork()` did a full deep copy instead of COW, how much extra physical memory would that take for a 128 MB region? Why is COW especially important for the common `fork()`/`exec()` pattern, where the child immediately replaces its entire address space anyway?

**Screenshot 4:** Complete output of `./cow`.

---

## Section 5 - Shared Libraries: One Copy for Everyone

When you compile a program the normal way, libc isn't baked into your binary. It's a shared library (`libc.so`) that gets memory-mapped into your process at load time. The nice thing about this: if 50 processes all use libc, the OS can keep just one copy of libc's code in physical memory and share those read-only pages across all of them. That saves a ton of RAM.

```bash
# What shared libraries does your shell depend on?
ldd /bin/bash

# Let's see the size difference between static and dynamic linking
cat << 'EOF' > hello.c
#include <stdio.h>
int main(void) {
    printf("Hello, shared world!\n");
    return 0;
}
EOF

gcc -o hello_dynamic hello.c
gcc -static -o hello_static hello.c

ls -lh hello_dynamic hello_static

# Watch the dynamic linker do its thing
LD_DEBUG=libs ./hello_dynamic 2>&1 | head -30
```

```bash
# How many processes are sharing libc right now on your system?
sudo grep 'libc' /proc/*/maps 2>/dev/null | awk '{print $NF}' | sort -u | head -5
echo "---"
echo "Processes sharing libc:"
sudo grep 'libc' /proc/*/maps 2>/dev/null | awk -F/ '{print $3}' | sort -un | wc -l
```

> **Q5.1:** How big is `hello_dynamic` vs `hello_static`? Why is the static binary so much larger?
>
> **Q5.2:** Run `cat /proc/self/maps | grep libc`. You'll see the same library mapped several times with different permissions - `r--p`, `r-xp`, `rw-p`. What does each one correspond to? (Think: read-only data, executable code, writable data.)
>
> **Q5.3:** If 50 processes all use libc, does the OS keep 50 copies of libc's code segment in RAM? What about libc's writable data (global variables) - is that shared too, or does each process get its own copy?

**Screenshot 5:** Output of `ls -lh hello_dynamic hello_static` and the first 30 lines of `LD_DEBUG=libs`.

---

## Section 6 - Under the Hood: How malloc Gets Memory from the Kernel

`malloc` is a userspace function - it's part of libc, not the kernel. But it needs to get memory from *somewhere*. Turns out it uses two different kernel syscalls depending on the size of the request:

```
  Small allocations (< 128 KB):       Large allocations (≥ 128 KB):
  ┌──────────────────────┐            ┌──────────────────────┐
  │     brk() syscall    │            │    mmap() syscall    │
  │                      │            │                      │
  │  Moves the "program  │            │  Creates a brand new │
  │  break" - the top    │            │  anonymous memory    │
  │  of the heap - up.   │            │  region, completely  │
  │                      │            │  separate from the   │
  │  ┌────────────────┐  │            │  heap.               │
  │  │  heap grows ↑  │  │            │                      │
  │  │  brk moves up  │  │            │  Can be munmap'd     │
  │  └────────────────┘  │            │  independently.      │
  └──────────────────────┘            └──────────────────────┘
```

```c
// alloc_trace.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    void *brk_before = sbrk(0);
    printf("Initial break:       %p\n\n", brk_before);

    // Small allocation → extends heap via brk()
    char *small = malloc(1024);
    printf("After malloc(1KB):   break=%p  ptr=%p  (break moved up)\n",
           sbrk(0), small);

    // Large allocation → separate region via mmap() (break should NOT move)
    char *large = malloc(256 * 1024);
    printf("After malloc(256KB): break=%p  ptr=%p  (break unchanged!)\n",
           sbrk(0), large);

    // Another small allocation → still on the heap via brk
    char *small2 = malloc(512);
    printf("After malloc(512B):  break=%p  ptr=%p\n",
           sbrk(0), small2);

    // small and small2 should be close together (both on the heap),
    // but large should be far away (it's in its own mmap region)
    printf("\nsmall  at %p\n", small);
    printf("small2 at %p  (close to small - both on heap)\n", small2);
    printf("large  at %p  (far away - mmap'd separately)\n", large);

    free(small);
    free(small2);
    free(large);
    return 0;
}
```

```bash
gcc -O0 -o alloc_trace alloc_trace.c
./alloc_trace

# Use strace to see the actual brk() and mmap() syscalls
strace -e brk,mmap ./alloc_trace 2>&1 | tail -20
```

> **Q6.1:** Does the program break move after `malloc(256 KB)`? Why not?
>
> **Q6.2:** In the `strace` output, which `malloc` call triggered a `brk` and which triggered an `mmap`? What's glibc's default threshold for switching from brk to mmap?
>
> **Q6.3:** Why bother with two strategies? Imagine everything used `brk`: you allocate A, B, C in order, then free B. The heap can't shrink past C, so B's space is stuck. That's heap fragmentation. How does `mmap` for large allocations sidestep this problem?

**Screenshot 6:** Output of `./alloc_trace` and the relevant `strace` lines.

---

## Section 7 - Fragmentation: Why Free Memory Isn't Always Usable

Two flavors of fragmentation come up constantly:

**External fragmentation** - you have plenty of free memory in total, but it's broken into small scattered chunks. A big allocation fails even though the free space technically exists, because none of the free chunks are big enough individually.

**Internal fragmentation** - each allocation wastes a little space due to alignment requirements and allocator metadata. You ask for 1 byte, the allocator actually reserves 16 or 32.

```
  External Fragmentation:
  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐
  │ USED │ │ free │ │ USED │ │ free │ │ USED │
  │  4KB │ │  4KB │ │  4KB │ │  4KB │ │  4KB │
  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘
  Total free = 8 KB, but largest contiguous = 4 KB!
  → A request for 8 KB fails even though 8 KB is free.

  Internal Fragmentation:
  ┌───────────────────┐
  │ requested: 100 B  │
  │ allocated: 128 B  │  ← 28 bytes wasted (alignment padding)
  └───────────────────┘
```

glibc's `malloc` is too smart to let us see external fragmentation easily (it uses mmap for big stuff, coalesces free chunks, etc.), so we'll build a simple pool allocator to make the problem obvious:

```c
// fragment.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL_SIZE (4 * 1024 * 1024)  // 4 MB pool
#define BLOCK_SIZE 4096              // 4 KB blocks
#define NUM_BLOCKS (POOL_SIZE / BLOCK_SIZE)  // 1024 blocks

int main(void) {
    char *pool = malloc(POOL_SIZE);
    if (!pool) { perror("malloc"); return 1; }

    // Track which blocks are "allocated"
    int allocated[NUM_BLOCKS];
    memset(allocated, 1, sizeof(allocated));  // all blocks "in use"

    printf("Pool: %d blocks of %d bytes = %d KB total\n",
           NUM_BLOCKS, BLOCK_SIZE, POOL_SIZE / 1024);

    // Free every OTHER block → creates a checkerboard: free-used-free-used-...
    int freed = 0;
    for (int i = 0; i < NUM_BLOCKS; i += 2) {
        allocated[i] = 0;
        freed++;
    }
    printf("Freed every other block: %d free blocks (%d KB free)\n",
           freed, freed * BLOCK_SIZE / 1024);

    // Now try to find a contiguous run of 128 blocks (512 KB)
    int need = 128;
    int found = 0;
    int run = 0;
    for (int i = 0; i < NUM_BLOCKS; i++) {
        if (!allocated[i]) {
            run++;
            if (run >= need) { found = 1; break; }
        } else {
            run = 0;
        }
    }

    printf("\nLooking for %d contiguous free blocks (%d KB)...\n",
           need, need * BLOCK_SIZE / 1024);
    if (found)
        printf("SUCCESS - found contiguous run\n");
    else
        printf("FAILED - %d KB free total, but largest contiguous run < %d KB\n",
               freed * BLOCK_SIZE / 1024, need * BLOCK_SIZE / 1024);
    printf("This is EXTERNAL FRAGMENTATION.\n");

    // Now show internal fragmentation
    printf("\n--- Internal Fragmentation ---\n");
    // malloc(1) actually reserves way more than 1 byte
    char *tiny = malloc(1);
    char *tiny2 = malloc(1);
    printf("malloc(1) at %p, malloc(1) at %p\n", tiny, tiny2);
    printf("Distance between them: %ld bytes (wasted: %ld bytes per alloc)\n",
           (long)(tiny2 - tiny), (long)(tiny2 - tiny) - 1);

    free(tiny);
    free(tiny2);
    free(pool);
    return 0;
}
```

```bash
gcc -O0 -o fragment fragment.c
./fragment
```

You should see 2048 KB free in total - but the 512 KB contiguous request fails because the free blocks are interleaved with used ones. And `malloc(1)` actually burns through 16–32 bytes per allocation.

> **Q7.1:** How much total memory is free? How much of it is contiguous? Why does the 512 KB request fail?
>
> **Q7.2:** glibc's allocator normally merges (coalesces) adjacent free chunks. Why does the checkerboard pattern - free, used, free, used - specifically prevent coalescing? (Draw it: no two free blocks are neighbors.)
>
> **Q7.3:** Paging eliminates external fragmentation for physical memory. Why? (A process needing 512 KB = 128 pages doesn't need 128 *contiguous* physical frames - the page table can map any virtual page to any physical frame, wherever it is.)

**Screenshot 7:** Output of `./fragment`.

---

## Section 8 - Swapping: When Physical RAM Runs Out

When all your physical memory is spoken for and something needs another page, the kernel has to make room. It picks some pages that haven't been used recently, writes them out to a swap area on disk, and frees up those frames for someone else. Those evicted pages are still part of the process's virtual address space - but the next time the process touches one of them, it triggers a major page fault, and the kernel has to read it back from disk. Disk is roughly 1000× slower than RAM, so this hurts.

```bash
# Check what swap you have
swapon --show
free -m
```

If you have no swap, set one up temporarily:

```bash
sudo fallocate -l 512M /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
swapon --show
```

```c
// swap_pressure.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MB (1024UL * 1024)

int main(void) {
    // Figure out how much RAM we have and allocate 80% of it
    FILE *f = fopen("/proc/meminfo", "r");
    unsigned long memtotal_kb;
    fscanf(f, "MemTotal: %lu kB", &memtotal_kb);
    fclose(f);

    unsigned long alloc_mb = (memtotal_kb / 1024) * 80 / 100;
    printf("Total RAM: %lu MB, allocating %lu MB (80%%)\n",
           memtotal_kb / 1024, alloc_mb);

    char *region = malloc(alloc_mb * MB);
    if (!region) { perror("malloc"); return 1; }

    // Touch every page so the kernel actually commits physical frames
    printf("Touching all pages...\n");
    for (unsigned long i = 0; i < alloc_mb * MB; i += 4096)
        region[i] = (char)(i & 0xFF);

    printf("Done. Sleeping 5s - run 'vmstat 1' in another terminal.\n");
    printf("PID: %d\n", getpid());
    fflush(stdout);
    sleep(5);

    // Re-read everything - some pages may have been swapped out by now
    printf("Re-reading all pages...\n");
    volatile char sink = 0;
    for (unsigned long i = 0; i < alloc_mb * MB; i += 4096)
        sink += region[i];

    printf("\nMemory status:\n");
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
        "grep -E 'VmRSS|VmSwap' /proc/%d/status", getpid());
    system(cmd);

    free(region);
    return 0;
}
```

You'll want two terminals for this:

**Terminal 1 (watch what the system is doing):**

```bash
vmstat 1
```

**Terminal 2 (apply the pressure):**

```bash
gcc -O0 -o swap_pressure swap_pressure.c
./swap_pressure
```

> **Q8.1:** In the `vmstat` output, the `si` and `so` columns show pages swapped in and out per second. Did you see non-zero values? When did they spike?
>
> **Q8.2:** Look at `VmSwap` in the output. If it's nonzero, that means some of this process's pages are currently sitting on disk instead of in RAM. Why did the kernel decide to swap them out?
>
> **Q8.3:** A major page fault (reading a page back from disk) takes about 5–10 ms. A minor fault (mapping a fresh zero page) takes about 1 μs. That's a ~10,000× difference. Why does this matter so much for database systems that try to keep their working set in RAM?

**Screenshot 8:** Side-by-side of `vmstat 1` during the run and the final VmRSS/VmSwap numbers.

---

## Section 9 - Page Replacement: Deciding What Gets Evicted

When RAM is full and a new page is needed, the kernel has to choose a victim. The theoretically perfect algorithm (Belady's OPT) would evict whichever page won't be used for the longest time - but that requires predicting the future, so it's useless in practice. Instead, Linux approximates LRU (Least Recently Used) using two linked lists:

```
  ┌─────────────────────────────────────────────────┐
  │                  Physical RAM                   │
  │                                                 │
  │  Active List          Inactive List             │
  │  (recently used)      (candidates for eviction) │
  │  ┌─┬─┬─┬─┬─┐         ┌─┬─┬─┬─┬─┐                │
  │  │A│B│C│D│E│         │F│G│H│I│J│                │
  │  └─┴─┴─┴─┴─┘         └─┴─┴─┴─┴─┘                │
  │       ↑                     │                   │
  │       │ promoted            │ evicted           │
  │       │ (accessed again)    ▼ (to swap/discard) │
  └─────────────────────────────────────────────────┘

  New pages start on the inactive list.
  If a page gets accessed again before it's evicted → promoted to active.
  If it just sits there untouched → eventually evicted.
```

The hardware helps out here: each PTE has an "accessed" bit that the CPU sets automatically whenever a page is read or written. The kernel periodically sweeps through and clears these bits, then checks later which ones got set again. This is basically a "second chance" / clock algorithm - it's not true LRU, but it's close enough and nearly free.

```bash
# See the kernel's current active/inactive page counts
grep -E 'Active|Inactive' /proc/meminfo
```

```c
// working_set.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MB (1024UL * 1024)
#define PAGE 4096

int main(int argc, char *argv[]) {
    unsigned long total_mb  = argc > 1 ? atol(argv[1]) : 256;
    unsigned long stride_mb = argc > 2 ? atol(argv[2]) : 64;
    int iterations = argc > 3 ? atoi(argv[3]) : 5;

    unsigned long total_bytes  = total_mb * MB;
    unsigned long stride_bytes = stride_mb * MB;

    char *region = malloc(total_bytes);
    if (!region) { perror("malloc"); return 1; }
    memset(region, 0, total_bytes);  // fault everything in

    printf("Total region: %lu MB, working set: %lu MB, iterations: %d\n\n",
           total_mb, stride_mb, iterations);

    // Only access the first stride_bytes repeatedly
    struct timespec start, end;
    for (int iter = 0; iter < iterations; iter++) {
        clock_gettime(CLOCK_MONOTONIC, &start);

        volatile char sink = 0;
        for (unsigned long off = 0; off < stride_bytes; off += PAGE)
            sink += region[off];

        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec)
                       + (end.tv_nsec - start.tv_nsec) / 1e9;
        printf("  iter %d: %.4f s  (%lu MB scanned)\n",
               iter, elapsed, stride_mb);
    }

    free(region);
    return 0;
}
```

```bash
gcc -O0 -o working_set working_set.c

# Small working set - fits comfortably in RAM
./working_set 512 64 5

# Working set = entire allocation - much more cache pressure
./working_set 512 512 5
```

> **Q9.1:** Compare the iteration times for the 64 MB working set vs the 512 MB one. Which is faster, and why?
>
> **Q9.2:** Run `grep -E 'Active|Inactive' /proc/meminfo` before and after each run. How do the active/inactive page counts shift?
>
> **Q9.3:** Why doesn't Linux use true LRU? (True LRU means updating a data structure on literally every memory access - that would be insanely expensive. The accessed bit in the PTE is set by hardware automatically, making the second-chance approximation almost free.)

**Screenshot 9:** Iteration timings for both working set sizes.

---

## Section 10 - Thrashing: The Performance Cliff

Thrashing is what happens when your working set exceeds available RAM and the system spends more time moving pages between RAM and disk than doing actual work. The scary part is that performance doesn't degrade gradually - it falls off a cliff. Things are fine, fine, fine, and then suddenly everything grinds to a halt.

**WARNING: This will make your VM very slow for 30–60 seconds. Save everything first..**

```c
// thrash.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MB (1024UL * 1024)
#define PAGE 4096

int main(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    unsigned long memtotal_kb;
    fscanf(f, "MemTotal: %lu kB", &memtotal_kb);
    fclose(f);
    unsigned long total_ram_mb = memtotal_kb / 1024;

    printf("Detected RAM: %lu MB\n", total_ram_mb);
    printf("%-12s  %8s  %12s  %10s\n",
           "Alloc(MB)", "Time(s)", "Throughput", "Status");
    printf("-----------------------------------------------------\n");

    // Ramp up: 25%, 50%, 75%, 100%, 130%, 160% of RAM
    int percents[] = {25, 50, 75, 100, 130, 160};
    int num_tests = sizeof(percents) / sizeof(percents[0]);

    for (int t = 0; t < num_tests; t++) {
        unsigned long target_mb = total_ram_mb * percents[t] / 100;
        unsigned long target_bytes = target_mb * MB;

        char *region = malloc(target_bytes);
        if (!region) {
            printf("%-12lu  FAILED (malloc returned NULL)\n", target_mb);
            continue;
        }

        // Touch all pages so they're backed by physical frames
        for (unsigned long i = 0; i < target_bytes; i += PAGE)
            region[i] = 1;

        // Random access - this is what kills you.
        // Sequential access lets the kernel prefetch; random access doesn't.
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        unsigned long num_pages = target_bytes / PAGE;
        unsigned long accesses = 100000;
        volatile char sink = 0;
        unsigned long seed = 12345;
        for (unsigned long i = 0; i < accesses; i++) {
            seed = seed * 6364136223846793005UL + 1;  // LCG PRNG
            unsigned long page_idx = (seed >> 16) % num_pages;
            sink += region[page_idx * PAGE];
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec)
                       + (end.tv_nsec - start.tv_nsec) / 1e9;
        double throughput = accesses / elapsed;

        const char *status;
        if (elapsed < 1.0)       status = "OK (in-RAM)";
        else if (elapsed < 10.0) status = "SLOW (swapping)";
        else                     status = "THRASHING";

        printf("%-12lu  %8.2f  %10.0f/s  %s\n",
               target_mb, elapsed, throughput, status);

        free(region);
        sleep(2);  // let things settle between runs
    }

    return 0;
}
```

```bash
gcc -O0 -o thrash thrash.c

# In another terminal, monitor with: vmstat 1
./thrash
```

> **Q10.1:** At what percentage of RAM did throughput collapse? Compare the numbers at 75% vs 130%.
>
> **Q10.2:** In `vmstat`, what happened to `si`/`so` (swap in/out) and `wa` (I/O wait %) once thrashing kicked in?
>
> **Q10.3:** Linux has two main defenses against thrashing:
>
> - **(a)** The OOM killer - what does it do, and when does it step in?
> - **(b)** `vm.swappiness` - run `cat /proc/sys/vm/swappiness`. What does this value (0–100) control?

**Screenshot 10:** The throughput table from `./thrash` and matching `vmstat` output.

---

## Cleanup

```bash
# Clean up all the binaries and source files
rm -f layout layout_pie demand pagemap cow hello_dynamic hello_static \
      alloc_trace fragment swap_pressure working_set thrash \
      layout.c demand.c pagemap.c cow.c hello.c alloc_trace.c \
      fragment.c swap_pressure.c working_set.c thrash.c
```

---

## Submission Checklist

### Screenshots (10 total)

| #   | What to capture                                              |
| --- | ------------------------------------------------------------ |
| 0   | `free -m` baseline                                           |
| 1   | `./layout` - addresses and filtered /proc/self/maps          |
| 2   | `./demand` - VmSize/VmRSS at each stage                      |
| 3   | `sudo ./pagemap` - present bits before/after touch           |
| 4   | `./cow` - page fault counts for read vs write passes         |
| 5   | Static vs dynamic binary sizes + `LD_DEBUG` output           |
| 6   | `./alloc_trace` output + `strace` brk/mmap lines             |
| 7   | `./fragment` output                                          |
| 8   | `vmstat` during swap pressure + VmRSS/VmSwap values          |
| 9   | `./working_set` iteration timings for both working set sizes |
| 10  | `./thrash` throughput table + `vmstat` during thrashing      |

### Written Answers (28 total)

| Section | Questions           |
| ------- | ------------------- |
| 0       | Q0.1                |
| 1       | Q1.1, Q1.2, Q1.3    |
| 2       | Q2.1, Q2.2, Q2.3    |
| 3       | Q3.1, Q3.2, Q3.3    |
| 4       | Q4.1, Q4.2, Q4.3    |
| 5       | Q5.1, Q5.2, Q5.3    |
| 6       | Q6.1, Q6.2, Q6.3    |
| 7       | Q7.1, Q7.2, Q7.3    |
| 8       | Q8.1, Q8.2, Q8.3    |
| 9       | Q9.1, Q9.2, Q9.3    |
| 10      | Q10.1, Q10.2, Q10.3 |
