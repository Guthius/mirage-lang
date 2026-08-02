// C side of tests/cdecl_abi_test.py. Calls INTO the Mirage '@(export, cdecl)' functions, so
// the argument marshalling is produced by clang's own System V implementation rather than by
// Mirage agreeing with itself.
#include <stdio.h>

typedef struct { int a, b; } Pair;
typedef struct { float x, y; } Vec2;
typedef struct { float x, y, z; } Vec3;
typedef struct { long long a, b, c, d; } Big;

int   mir_pair_sum(Pair p);
Pair  mir_pair_make(int a, int b);
float mir_vec2_sum(Vec2 v);
Vec2  mir_vec2_make(float x, float y);
float mir_vec3_sum(Vec3 v);
Vec3  mir_vec3_make(float x, float y, float z);
long long mir_big_sum(Big b);
Big   mir_big_make(long long a, long long b, long long c, long long d);
int   mir_mixed(int lead, Pair p, int trail);

static int failures = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        failures++;
        fprintf(stderr, "cdecl ABI FAIL: %s\n", what);
    }
}

int run_c_side(void) {
    Pair p = {3, 4};
    check(mir_pair_sum(p) == 7, "Pair passed by value (one INTEGER eightbyte)");

    Pair made = mir_pair_make(11, 22);
    check(made.a == 11 && made.b == 22, "Pair returned by value");

    Vec2 v2 = {1.5f, 2.25f};
    check(mir_vec2_sum(v2) == 3.75f, "Vec2 passed by value (one SSE eightbyte)");

    Vec2 v2m = mir_vec2_make(5.5f, 6.25f);
    check(v2m.x == 5.5f && v2m.y == 6.25f, "Vec2 returned by value");

    Vec3 v3 = {1.0f, 2.0f, 4.0f};
    check(mir_vec3_sum(v3) == 7.0f, "Vec3 passed by value (two eightbytes)");

    Vec3 v3m = mir_vec3_make(9.0f, 8.0f, 7.0f);
    check(v3m.x == 9.0f && v3m.y == 8.0f && v3m.z == 7.0f, "Vec3 returned by value");

    Big b = {1, 2, 3, 4};
    check(mir_big_sum(b) == 10, "Big passed by value (MEMORY class, byval)");

    Big bm = mir_big_make(100, 200, 300, 400);
    check(bm.a == 100 && bm.b == 200 && bm.c == 300 && bm.d == 400, "Big returned by value (sret)");

    check(mir_mixed(1, p, 10) == 18, "aggregate between two scalar arguments");

    return failures;
}
