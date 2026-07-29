/* C side of the ext-fn ABI test. Compiled by tests/ext_abi_test.py with the system clang,
   so the values these functions see are produced by a real, independent SysV implementation
   rather than by Mirage's own idea of the ABI. */
#include <stdint.h>

/* Mirage passes '[2]f32' by value. The C type with the same SysV classification is a struct
   wrapping float[2] -- both are one eightbyte of SSE class, lowered to <2 x float>. */
struct ArrWrap { float a[2]; };
float sum_arr(struct ArrWrap w) { return w.a[0] * 10.0f + w.a[1]; }

/* > 16 bytes: MEMORY class, passed indirectly with byval. */
struct Big { int64_t a, b, c; };
int64_t sum_big(struct Big b) { return b.a * 100 + b.b * 10 + b.c; }

/* Exactly 16 bytes of INTEGER class: two eightbytes in registers. */
struct TwoWords { int64_t lo, hi; };
int64_t sum_two_words(struct TwoWords t) { return t.lo * 10 + t.hi; }

/* Packed, so 'b' spans bytes 1..9 and straddles the eightbyte boundary. SysV forces the whole
   struct to MEMORY class; clang passes it as 'ptr byval(...) align 8'. Classifying the two
   halves of 'b' independently would conclude it travels in two registers instead. */
struct __attribute__((packed)) Straddle { unsigned char a; int64_t b; };
int64_t sum_straddle(struct Straddle s) { return s.a * 1000 + s.b; }
