/* Guest address space, image loader, heap and indirect dispatch. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "rt.h"

uint32_t R[16];
uint32_t NF, ZF, CF, VF;
uint32_t FNF, FZF, FCF, FVF, FPSCR_REST;
union fpregs FP;
uint8_t *MEM;

/* ------------------------------------------------------------------ loader */

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = n;
    return b;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

void mem_init(const char *exe_path) {
    MEM = mmap(NULL, (size_t)1 << 32, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (MEM == MAP_FAILED) { perror("mmap guest space"); exit(1); }
    size_t len;
    uint8_t *f = read_file(exe_path, &len);
    if (!f) { fprintf(stderr, "cannot read game executable %s\n", exe_path); exit(1); }
    const uint8_t *img = f;
    size_t img_len = len;
    if (be32(f) == 0xcafebabe) {          /* fat binary: pick the armv6 slice */
        uint32_t n = be32(f + 4);
        img = NULL;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *a = f + 8 + 20 * i;
            if (be32(a) == 12 && be32(a + 4) == 6) { img = f + be32(a + 8); img_len = be32(a + 12); }
        }
        if (!img) { fprintf(stderr, "no armv6 slice in executable\n"); exit(1); }
    }
    for (unsigned i = 0; i < guest_seg_count; i++) {
        const struct guest_seg *s = &guest_segs[i];
        if (s->filesize && s->fileoff + s->filesize <= img_len)
            memcpy(MEM + s->vmaddr, img + s->fileoff, s->filesize);
    }
    free(f);
}

/* ------------------------------------------------------------------ heap */
/* Simple segregated-fit allocator living in guest memory. */

#define HEAP_BASE 0x10000000u
#define HEAP_END  0xD0000000u
#define STACK_TOP 0xE1000000u
#define NCLASS 40

static uint32_t heap_brk = HEAP_BASE;
static uint32_t free_lists[NCLASS];

static int size_class(uint32_t n) {
    int c = 0;
    uint32_t s = 16;
    while (s < n) { s <<= 1; c++; }
    return c;
}

uint32_t gmalloc(uint32_t n) {
    if (n == 0) n = 1;
    int c = size_class(n + 8);
    uint32_t bsize = 16u << c;
    uint32_t p;
    if (free_lists[c]) {
        p = free_lists[c];
        free_lists[c] = LD32(p);
    } else {
        heap_brk = (heap_brk + 15) & ~15u;
        if ((uint64_t)heap_brk + bsize > HEAP_END) { fprintf(stderr, "guest heap exhausted\n"); abort(); }
        p = heap_brk;
        heap_brk += bsize;
    }
    ST32(p, c);
    ST32(p + 4, n);
    return p + 8;
}

uint32_t gcalloc(uint32_t n) {
    uint32_t p = gmalloc(n);
    memset(GPTR(p), 0, n);
    return p;
}

void gfree(uint32_t p) {
    if (!p) return;
    uint32_t h = p - 8;
    uint32_t c = LD32(h);
    if (c >= NCLASS) { fprintf(stderr, "gfree: bad pointer %08x\n", p); return; }
    ST32(h, free_lists[c]);
    free_lists[c] = h;
}

uint32_t gsize(uint32_t p) { return p ? LD32(p - 4) : 0; }

uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = gmalloc(n);
    memcpy(GPTR(p), s, n);
    return p;
}

void stack_init(void) { R[13] = STACK_TOP - 64; }

/* ------------------------------------------------------------------ dispatch */

static void (*lookup_fn(uint32_t addr))(void) {
    unsigned lo = 0, hi = guest_fn_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (guest_fn_table[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < guest_fn_count && guest_fn_table[lo].addr == addr) return guest_fn_table[lo].fn;
    return NULL;
}

/* host functions handed out to the guest as function pointers (alcGetProcAddress) */
#define HOSTFN_BASE 0xF0000000u
static void (*hostfns[64])(void);
static int nhostfns;

uint32_t register_host_fn(void (*fn)(void)) {
    for (int i = 0; i < nhostfns; i++)
        if (hostfns[i] == fn) return HOSTFN_BASE + 4 * i;
    hostfns[nhostfns] = fn;
    return HOSTFN_BASE + 4 * nhostfns++;
}

void call_indirect(uint32_t addr) {
    if (addr >= HOSTFN_BASE && addr < HOSTFN_BASE + 4 * (uint32_t)nhostfns) { hostfns[(addr - HOSTFN_BASE) / 4](); return; }
    void (*fn)(void) = lookup_fn(addr);
    if (!fn) {
        fprintf(stderr, "call_indirect: no function at %08x (lr=%08x)\n", addr, R[14]);
        abort();
    }
    fn();
}

void jump_indirect(uint32_t addr) { call_indirect(addr); }

void guest_trap(uint32_t addr) {
    fprintf(stderr, "guest trap at %08x\n", addr);
    abort();
}

void guest_bad_call(uint32_t from, uint32_t to) {
    fprintf(stderr, "bad call from %08x to %08x\n", from, to);
    abort();
}

void guest_bad_jump(uint32_t from, uint32_t to) {
    fprintf(stderr, "bad computed jump at %08x to %08x\n", from, to);
    abort();
}

/* Call a guest function with up to 8 word arguments; returns r0 (r1 in *hi). */
uint32_t guest_call(uint32_t addr, int nargs, const uint32_t *args, uint32_t *hi) {
    uint32_t saved[16];
    memcpy(saved, R, sizeof saved);
    uint32_t sp = R[13];
    int nstack = nargs > 4 ? nargs - 4 : 0;
    sp -= 4 * nstack;
    sp &= ~7u;
    for (int i = 0; i < nstack; i++) ST32(sp + 4 * i, args[4 + i]);
    for (int i = 0; i < 4 && i < nargs; i++) R[i] = args[i];
    R[13] = sp;
    R[14] = 0;
    call_indirect(addr);
    uint32_t r0 = R[0];
    if (hi) *hi = R[1];
    /* restore callee-saved registers and stack as a real call would leave them */
    for (int i = 4; i < 16; i++) R[i] = saved[i];
    return r0;
}
