/* Guest CPU state and helpers shared by the recompiled code and the runtime. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

extern uint32_t R[16];
extern uint32_t NF, ZF, CF, VF;
extern uint32_t FNF, FZF, FCF, FVF, FPSCR_REST;
extern union fpregs { float s[32]; double d[16]; uint32_t u[32]; } FP;
#define FS FP.s
#define FD FP.d
#define FU FP.u

extern uint8_t *MEM;

typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef int16_t __attribute__((aligned(1), may_alias)) s16u;

#define GPTR(a) ((void *)(MEM + (uint32_t)(a)))
#define LD32(a) (*(u32u *)(MEM + (uint32_t)(a)))
#define LD16(a) ((uint32_t)*(u16u *)(MEM + (uint32_t)(a)))
#define LDS16(a) ((uint32_t)(int32_t)*(s16u *)(MEM + (uint32_t)(a)))
#define LD8(a) ((uint32_t)MEM[(uint32_t)(a)])
#define LDS8(a) ((uint32_t)(int32_t)(int8_t)MEM[(uint32_t)(a)])
#define ST32(a, v) (*(u32u *)(MEM + (uint32_t)(a)) = (uint32_t)(v))
#define ST16(a, v) (*(u16u *)(MEM + (uint32_t)(a)) = (uint16_t)(v))
#define ST8(a, v) (MEM[(uint32_t)(a)] = (uint8_t)(v))

static inline uint32_t ROR32(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

static inline uint32_t ADDC(uint32_t a, uint32_t b, uint32_t c) {
    uint64_t r = (uint64_t)a + b + c;
    uint32_t res = (uint32_t)r;
    CF = (uint32_t)(r >> 32);
    VF = ((a ^ res) & (b ^ res)) >> 31;
    NF = res >> 31;
    ZF = res == 0;
    return res;
}

static inline uint32_t SH_LSL(uint32_t v, uint32_t s) { return s >= 32 ? 0 : v << s; }
static inline uint32_t SH_LSR(uint32_t v, uint32_t s) { return s >= 32 ? 0 : v >> s; }
static inline uint32_t SH_ASR(uint32_t v, uint32_t s) { return (uint32_t)((int32_t)v >> (s >= 32 ? 31 : s)); }
static inline uint32_t SH_ROR(uint32_t v, uint32_t s) { return ROR32(v, s & 31); }
static inline uint32_t SHC_LSL(uint32_t v, uint32_t s) { return s == 0 ? CF : s < 32 ? (v >> (32 - s)) & 1 : s == 32 ? v & 1 : 0; }
static inline uint32_t SHC_LSR(uint32_t v, uint32_t s) { return s == 0 ? CF : s <= 32 ? (v >> (s - 1)) & 1 : 0; }
static inline uint32_t SHC_ASR(uint32_t v, uint32_t s) { return s == 0 ? CF : s < 32 ? (v >> (s - 1)) & 1 : v >> 31; }
static inline uint32_t SHC_ROR(uint32_t v, uint32_t s) { return s == 0 ? CF : (v >> (((s & 31) + 31) & 31)) & 1; }
static inline uint32_t CLZ32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline uint32_t USAT(int32_t v, int n) { int32_t mx = (int32_t)((1u << n) - 1); return v < 0 ? 0 : v > mx ? (uint32_t)mx : (uint32_t)v; }
static inline uint32_t SSAT(int32_t v, int n) { int32_t mx = (int32_t)((1u << (n - 1)) - 1), mn = -mx - 1; return (uint32_t)(v < mn ? mn : v > mx ? mx : v); }

#define VFP_CMP(a, b) do { \
    if ((a) != (a) || (b) != (b)) { FNF = 0; FZF = 0; FCF = 1; FVF = 1; } \
    else if ((a) == (b)) { FNF = 0; FZF = 1; FCF = 1; FVF = 0; } \
    else if ((a) < (b)) { FNF = 1; FZF = 0; FCF = 0; FVF = 0; } \
    else { FNF = 0; FZF = 0; FCF = 1; FVF = 0; } } while (0)

static inline uint32_t VCVT_SZ_D(double d) { if (d != d) return 0; if (d >= 2147483647.0) return 0x7fffffff; if (d <= -2147483648.0) return 0x80000000u; return (uint32_t)(int32_t)d; }
static inline uint32_t VCVT_UZ_D(double d) { if (d != d || d <= 0) return 0; if (d >= 4294967295.0) return 0xffffffffu; return (uint32_t)d; }
static inline uint32_t VCVT_SR_D(double d) { return VCVT_SZ_D(nearbyint(d)); }
static inline uint32_t VCVT_UR_D(double d) { return VCVT_UZ_D(nearbyint(d)); }
#define VCVT_SZ_F(f) VCVT_SZ_D((double)(f))
#define VCVT_UZ_F(f) VCVT_UZ_D((double)(f))
#define VCVT_SR_F(f) VCVT_SR_D((double)(f))
#define VCVT_UR_F(f) VCVT_UR_D((double)(f))

/* tables emitted by the recompiler */
struct guest_fn { uint32_t addr; void (*fn)(void); };
struct guest_bind { uint32_t addr; const char *sym; uint32_t stub; };
struct guest_seg { const char *name; uint32_t vmaddr, vmsize, fileoff, filesize; };
extern const struct guest_fn guest_fn_table[];
extern const unsigned guest_fn_count;
extern const struct guest_bind guest_binds[];
extern const unsigned guest_bind_count;
extern const struct guest_seg guest_segs[];
extern const unsigned guest_seg_count;
extern const uint32_t guest_entry;

void call_indirect(uint32_t addr);
void jump_indirect(uint32_t addr);
void guest_trap(uint32_t addr) __attribute__((noreturn));
void guest_bad_call(uint32_t from, uint32_t to);
void guest_bad_jump(uint32_t from, uint32_t to);
