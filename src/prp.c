// Fermat PRP test of N = k*2^n - 1 with Gerbicz error checking, round-off
// fallback to a larger FFT, and checkpointing.
//
// Chain: x_0 = base^k, x_{i+1} = x_i^2, so x_n = base^(k*2^n) = base^(N+1).
// The residue is base^(N-1) = x_n / base^2 (mod N); N is a PRP iff
// x_n == base^2.
//
// Gerbicz check with block size B: d_t = prod_{j<=t} x_{jB} satisfies
// d_{t+1} = x_0 * d_t^(2^B). d is updated every B squarings; every M blocks
// the identity is verified with B extra squarings (overhead ~1/B + 1/M). A
// failed check rolls back to the last verified (i, x, d).
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Add 1 to one digit: a valid representation of a wrong value, which only
// the Gerbicz check can catch.
void kite_debug_corrupt(kite_ctx *ctx, kite_num *x) {
    int64_t *dig = malloc(ctx->L * sizeof(int64_t));
    ctx->ops->to_digits(ctx, x, dig);
    dig[ctx->L / 3] += 1;
    ctx->ops->from_digits(ctx, x, dig);
    free(dig);
}

// ---- small bigint helpers -------------------------------------------------

static uint64_t limbs_mod_small(const uint64_t *a, size_t n, uint64_t m) {
    unsigned __int128 r = 0;
    for (size_t i = n; i-- > 0;) r = ((r << 64) | a[i]) % m;
    return (uint64_t)r;
}

// a = a / d mod N (a < N, d small and coprime to N)
static void div_small_mod_N(uint64_t *a, const uint64_t *N, size_t n, uint64_t d) {
    uint64_t am = limbs_mod_small(a, n, d), nm = limbs_mod_small(N, n, d);
    uint64_t t = 0;
    while ((am + t * nm) % d) t++;
    unsigned __int128 c = 0;
    for (size_t i = 0; i < n; i++) {
        c += (unsigned __int128)a[i] + (unsigned __int128)N[i] * t;
        a[i] = (uint64_t)c;
        c >>= 64;
    }
    unsigned __int128 r = c;
    for (size_t i = n; i-- > 0;) {
        r = (r << 64) | a[i];
        a[i] = (uint64_t)(r / d);
        r %= d;
    }
}

// y = base^(N+1) mod N  ->  base^(N-1) mod N, also when gcd(base, N) > 1:
// write N = Q*M with Q built from the primes of base; the result is 0 mod Q
// and y / base^2 mod M, combined by CRT. Returns -1 if Q does not fit.
int kite_residue_nm1(uint64_t *y, const uint64_t *N, size_t n, uint64_t base) {
    uint64_t Q = 1, b = base;
    uint64_t *M = malloc((n + 1) * 8), *t = calloc(n + 1, 8);
    memcpy(M, N, n * 8);
    M[n] = 0;
    for (uint64_t p = 2; b > 1; p++) {
        if (b % p) continue;
        while (b % p == 0) b /= p;
        while (limbs_mod_small(M, n, p) == 0) {
            if (Q > (1ull << 40)) {
                free(M);
                free(t);
                return -1;
            }
            Q *= p;
            unsigned __int128 r = 0;
            for (size_t i = n; i-- > 0;) {
                r = (r << 64) | M[i];
                M[i] = (uint64_t)(r / p);
                r %= p;
            }
        }
    }
    if (Q == 1) {
        div_small_mod_N(y, N, n, base);
        div_small_mod_N(y, N, n, base);
        free(M);
        free(t);
        return 0;
    }
    // r = y mod M = (y*Q - q*N) / Q with q = floor(y*Q / N) < Q
    unsigned __int128 c = 0;
    for (size_t i = 0; i < n; i++) {
        c += (unsigned __int128)y[i] * Q;
        t[i] = (uint64_t)c;
        c >>= 64;
    }
    t[n] = (uint64_t)c;
    for (;;) {
        int ge = t[n] != 0;
        if (!ge) {
            ge = 1;
            for (size_t i = n; i-- > 0;) {
                if (t[i] != N[i]) {
                    ge = t[i] > N[i];
                    break;
                }
            }
        }
        if (!ge) break;
        uint64_t borrow = 0;
        for (size_t i = 0; i <= n; i++) {
            unsigned __int128 d = (unsigned __int128)t[i] - (i < n ? N[i] : 0) - borrow;
            t[i] = (uint64_t)d;
            borrow = (uint64_t)(d >> 64) & 1;
        }
    }
    unsigned __int128 r = 0;
    for (size_t i = n + 1; i-- > 0;) {
        r = (r << 64) | t[i];
        t[i] = (uint64_t)(r / Q);
        r %= Q;
    }
    // r / base^2 mod M (base is coprime to M)
    div_small_mod_N(t, M, n, base);
    div_small_mod_N(t, M, n, base);
    // x = t + M*u with x == 0 mod Q
    uint64_t rm = limbs_mod_small(t, n, Q), mm = limbs_mod_small(M, n, Q), u = 0;
    while ((rm + (unsigned __int128)mm * u) % Q) u++;
    c = 0;
    for (size_t i = 0; i < n; i++) {
        c += (unsigned __int128)t[i] + (unsigned __int128)M[i] * u;
        y[i] = (uint64_t)c;
        c >>= 64;
    }
    free(M);
    free(t);
    return 0;
}

// FNV-1a 64 over bytes
static uint64_t fnv(const void *p, size_t len, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < len; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// ---- checkpoint -----------------------------------------------------------

typedef struct {
    char magic[8];
    uint64_t k, n, base, iter, nlimbs, fft_len;
} ckpt_hdr;

// Written as "KITECKP1". Releases up to 0.1.3 wrote "AHNCKPT1", the project's name before it was
// kite, while reading only "KITECKP1", so no Fermat test ever resumed. The layout is the same, so
// both are read and a test interrupted under 0.1.3 picks up where it was.
static const char CKPT_MAGIC[8] = {'K', 'I', 'T', 'E', 'C', 'K', 'P', '1'};
static const char CKPT_MAGIC_OLD[8] = {'A', 'H', 'N', 'C', 'K', 'P', 'T', '1'};

static int ckpt_magic_ok(const char magic[8]) {
    return !memcmp(magic, CKPT_MAGIC, 8) || !memcmp(magic, CKPT_MAGIC_OLD, 8);
}

static int ckpt_write(const char *path, uint64_t k, uint64_t n, uint32_t base, uint64_t iter,
                      uint32_t fft_len, const uint64_t *x, const uint64_t *d, size_t nl) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    ckpt_hdr h = {{0}, k, n, base, iter, nl, fft_len};
    memcpy(h.magic, CKPT_MAGIC, 8);
    uint64_t sum = fnv(&h, sizeof h, 0xcbf29ce484222325ull);
    sum = fnv(x, nl * 8, sum);
    sum = fnv(d, nl * 8, sum);
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(x, 8, nl, f) == nl &&
             fwrite(d, 8, nl, f) == nl && fwrite(&sum, 8, 1, f) == 1;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path)) {
        remove(tmp);
        return -1;
    }
    return 0;
}

static int ckpt_read(const char *path, uint64_t k, uint64_t n, uint32_t base, uint64_t *iter,
                     uint32_t *fft_len, uint64_t *x, uint64_t *d, size_t nl) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    ckpt_hdr h;
    uint64_t sum = 0;
    int ok = fread(&h, sizeof h, 1, f) == 1 && ckpt_magic_ok(h.magic) && h.k == k &&
             h.n == n && h.base == base && h.nlimbs == nl && fread(x, 8, nl, f) == nl &&
             fread(d, 8, nl, f) == nl && fread(&sum, 8, 1, f) == 1;
    fclose(f);
    if (!ok) return -1;
    uint64_t s = fnv(&h, sizeof h, 0xcbf29ce484222325ull);
    s = fnv(x, nl * 8, s);
    s = fnv(d, nl * 8, s);
    if (s != sum) return -1;
    *iter = h.iter;
    *fft_len = (uint32_t)h.fft_len;
    return 0;
}

// ---- proof points -----------------------------------------------------------

// Points captured since the last verified state, reported once verified.
typedef struct {
    uint32_t *index;
    uint64_t **limbs;
    uint32_t count, cap;
} pending_points;

static void pending_clear(pending_points *p) {
    for (uint32_t i = 0; i < p->count; i++) free(p->limbs[i]);
    p->count = 0;
}

static void pending_add(pending_points *p, kite_ctx *ctx, uint32_t index, const kite_num *x) {
    if (p->count == p->cap) {
        p->cap = p->cap ? 2 * p->cap : 16;
        p->index = realloc(p->index, p->cap * sizeof *p->index);
        p->limbs = realloc(p->limbs, p->cap * sizeof *p->limbs);
    }
    p->index[p->count] = index;
    p->limbs[p->count] = malloc(kite_limbs(ctx) * 8);
    kite_get_limbs(ctx, x, p->limbs[p->count]);
    p->count++;
}

static void pending_truncate(pending_points *p, uint32_t count) {
    while (p->count > count) free(p->limbs[--p->count]);
}

static int pending_flush(pending_points *p, const kite_prp_opts *o) {
    int rc = 0;
    for (uint32_t i = 0; i < p->count && !rc; i++)
        rc = o->on_point(o->point_arg, p->index[i], o->points[p->index[i]], p->limbs[i]);
    pending_clear(p);
    return rc;
}

int kite_prp_checkpoint_iter(const char *path, uint64_t k, uint64_t n, uint32_t base,
                            uint64_t *iter) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    ckpt_hdr h;
    int ok = fread(&h, sizeof h, 1, f) == 1 && ckpt_magic_ok(h.magic) && h.k == k &&
             h.n == n && h.base == base;
    fclose(f);
    if (!ok) return -1;
    *iter = h.iter;
    return 0;
}

// ---- driver ---------------------------------------------------------------

typedef struct {
    kite_ctx *ctx;
    kite_num *x, *d, *x0, *t;
    // careful operations: r is a fixed pseudo-random number, r2 = r^2; c1, c2 scratch; b holds
    // the value at the start of a block for a replay
    kite_num *r, *r2, *c1, *c2, *b;
    double maxround;
    uint32_t careful;  // careful operations done
} prp_state;

static void state_free(prp_state *s) {
    if (!s->ctx) return;
    kite_num *nums[] = {s->x, s->d, s->x0, s->t, s->r, s->r2, s->c1, s->c2, s->b};
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++)
        if (nums[i]) kite_num_free(s->ctx, nums[i]);
    kite_ctx_free(s->ctx);
    memset(s, 0, sizeof *s);
}

// Bring a sum of two stored numbers back to balanced digits (kite_add leaves them unbalanced,
// which would raise the round-off of the multiplication that follows).
static void normalise(prp_state *s, kite_num *x) { kite_mul_small(s->ctx, x, 1); }

// x = x^2 as (x + r)(x - r) + r^2: the same value from a different multiplication, so its
// round-off is a new draw. gwnum's gwmul3_carefully does the same; PRST redoes an operation with
// it when its round-off is over the limit, at the same FFT length.
static void careful_square(prp_state *s, kite_num *x) {
    kite_copy(s->ctx, s->c1, x);
    kite_add(s->ctx, s->c1, s->r);
    normalise(s, s->c1);
    kite_copy(s->ctx, s->c2, x);
    kite_sub(s->ctx, s->c2, s->r);
    normalise(s, s->c2);
    kite_mul(s->ctx, s->c1, s->c2);
    kite_add(s->ctx, s->c1, s->r2);
    normalise(s, s->c1);
    kite_copy(s->ctx, x, s->c1);
    s->careful++;
}

// x = a * y as (a + r) y - r y (x may be a; y must not be x)
static void careful_mul(prp_state *s, kite_num *x, const kite_num *a, const kite_num *y) {
    kite_copy(s->ctx, s->c1, a);
    kite_add(s->ctx, s->c1, s->r);
    normalise(s, s->c1);
    kite_mul(s->ctx, s->c1, y);
    kite_copy(s->ctx, s->c2, s->r);
    kite_mul(s->ctx, s->c2, y);
    kite_sub(s->ctx, s->c1, s->c2);
    normalise(s, s->c1);
    kite_copy(s->ctx, x, s->c1);
    s->careful++;
}

// A careful redo costs ~22 squarings (a replay of 16 on average plus ~6 for the operation), so
// more than B*M/1100 in one round cost more than 2% of the round and the test moves to a longer
// FFT. A flat 32 moved tests up at ~0.1% cost: near the limit the redos come evenly, ~200 per
// million squarings at 64K 0.20 bits per word past the fit (2026-10-18).
#define CAREFUL_COST_DIV 1100

// The first squarings from x_0 = base^k (a number of a few thousand bits) work on data that is
// far from random, with round-off well above what the FFT length was chosen for; gwnum's author
// squares the first 30 carefully in his own PRP code, and PRST does (gwset_carefully_count).
#define CAREFUL_START 30

// x = x^2 for iteration i + 1 of the test: carefully for the first CAREFUL_START
static void square_at(prp_state *s, kite_num *x, uint64_t i) {
    if (i < CAREFUL_START) {
        careful_square(s, x);
        s->careful--;  // planned, not a redo
    } else {
        kite_square(s->ctx, x);
    }
}

static int over(const prp_state *s) { return kite_max_roundoff(s->ctx) > s->maxround; }

// x = x^(2^count), each squaring checked: one over the limit is replayed from the last snapshot
// (every 32 squarings) and done carefully. Returns 0, or -1 if a careful squaring is over too.
static int square_n_checked(prp_state *s, kite_num *x, uint64_t count) {
    uint64_t jb = 0;
    for (uint64_t j = 0; j < count; j++) {
        if (j % 32 == 0) {
            kite_copy(s->ctx, s->b, x);
            jb = j;
        }
        kite_square(s->ctx, x);
        if (!over(s)) continue;
        kite_reset_roundoff(s->ctx);
        kite_copy(s->ctx, x, s->b);
        for (uint64_t jj = jb; jj < j; jj++) kite_square(s->ctx, x);
        careful_square(s, x);
        if (over(s)) return -1;
    }
    return 0;
}

static int state_new(prp_state *s, uint64_t k, uint64_t n, kite_opts o, double maxround) {
    s->ctx = kite_ctx_new(k, n, -1, &o);
    if (!s->ctx) return -1;
    s->maxround = maxround;
    // Past the safe-bpw margin (an fft.edge length) check every operation, as PRST does near its
    // FFT limit, so an over-limit one is redone at once; otherwise sample every 4th, enough to
    // spot an FFT length that is too short. Arithmetic errors are caught by Gerbicz either way.
    uint32_t L = kite_fft_len(s->ctx);
    kite_set_roundoff_check(s->ctx, (double)n / L > kite_safe_bpw(L, k) ? 1 : 4);
    kite_set_sum_check(s->ctx, 1);
    kite_num **nums[] = {&s->x, &s->d, &s->x0, &s->t, &s->r, &s->r2, &s->c1, &s->c2, &s->b};
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        *nums[i] = kite_num_new(s->ctx);
        if (!*nums[i]) return -1;
    }
    // r: fixed pseudo-random bits below 2^(n-1), so r < N
    size_t nl = kite_limbs(s->ctx);
    uint64_t *l = calloc(nl, 8), z = 0x9E3779B97F4A7C15ull;
    if (!l) return -1;
    for (size_t i = 0; i < nl; i++) {
        z ^= z << 13, z ^= z >> 7, z ^= z << 17;
        l[i] = z;
    }
    for (uint64_t bit = n - 1; bit < 64 * nl; bit++) l[bit / 64] &= ~(1ull << (bit % 64));
    kite_set_limbs(s->ctx, s->r, l);
    free(l);
    kite_copy(s->ctx, s->r2, s->r);
    kite_square(s->ctx, s->r2);
    kite_reset_roundoff(s->ctx);
    return 0;
}

static int nums_equal(kite_ctx *ctx, const kite_num *a, const kite_num *b, uint64_t *la,
                      uint64_t *lb) {
    kite_get_limbs(ctx, a, la);
    kite_get_limbs(ctx, b, lb);
    return !memcmp(la, lb, kite_limbs(ctx) * 8);
}

// base^k mod N into limbs (exact power while it is below N, then reduce
// using the context arithmetic).
static void power_start(prp_state *s, uint64_t k, uint32_t base, uint64_t *l) {
    kite_ctx *ctx = s->ctx;
    size_t nl = kite_limbs(ctx);
    // left-to-right binary powering with small multiplications
    kite_set_u64(ctx, s->x0, 1);
    for (int b = 63; b >= 0; b--) {
        if (k >> b == 0) continue;
        kite_square(ctx, s->x0);
        if ((k >> b) & 1) kite_mul_small(ctx, s->x0, base);
    }
    // verify against an independent computation when base^k < N
    double bits = k * log2((double)base);
    if (bits + 2 < (double)ctx->n) {
        memset(l, 0, nl * 8);
        l[0] = 1;
        size_t used = 1;
        for (uint64_t i = 0; i < k; i++) {
            unsigned __int128 c = 0;
            for (size_t j = 0; j < used; j++) {
                c += (unsigned __int128)l[j] * base;
                l[j] = (uint64_t)c;
                c >>= 64;
            }
            if (c) l[used++] = (uint64_t)c;
        }
        kite_set_limbs(ctx, s->x0, l);
    }
}

int kite_prp(uint64_t k, uint64_t n, uint32_t base, const kite_prp_opts *opts,
            kite_prp_result *res) {
    kite_prp_opts o = opts ? *opts : (kite_prp_opts){0};
    // the round-off recovery below lets an automatic length sit at the fitted limit
    if (!o.fft.fft_len) o.fft.edge = 1;
    memset(res, 0, sizeof *res);
    double t_start = now();
    uint32_t B = o.gerbicz_block;
    if (!B) {
        B = (uint32_t)sqrt((double)n);
        if (B < 32) B = 32;
        if (B > 2000) B = 2000;
    }
    uint32_t M = o.gerbicz_blocks ? o.gerbicz_blocks : (B / 4 > 8 ? B / 4 : 8);
    uint32_t careful_limit = o.careful_per_round ? o.careful_per_round
                                                 : (uint32_t)((uint64_t)B * M / CAREFUL_COST_DIV) + 8;
    double maxround = o.max_roundoff > 0 ? o.max_roundoff : 0.40;
    double ck_secs = o.checkpoint_secs > 0 ? o.checkpoint_secs : 600;
    double u_secs = o.unverified_secs > 0 ? o.unverified_secs : 60;

    prp_state s = {0};
    if (state_new(&s, k, n, o.fft, maxround)) {
        res->careful_ops += s.careful;
        if (s.ctx) res->sum_errors += kite_sum_errors(s.ctx);
        state_free(&s);
        return -1;
    }
    size_t nl = kite_limbs(s.ctx);
    uint64_t *la = calloc(nl, 8), *lb = calloc(nl, 8);
    uint64_t *vx = calloc(nl, 8), *vd = calloc(nl, 8);  // last verified x, d
    uint64_t vi = 0;                                     // last verified iteration
    uint64_t *ux = NULL, *ud = NULL, ui = 0;             // unverified state to resume from
    int rc = 0;
    pending_points pend = {0};

    power_start(&s, k, base, la);
    kite_get_limbs(s.ctx, s.x0, vx);
    memcpy(vd, vx, nl * 8);
    {
        uint32_t L = 0, uL = 0;
        if (o.checkpoint && !ckpt_read(o.checkpoint, k, n, base, &vi, &L, vx, vd, nl)) res->resumed = 1;
        // The unverified file is newer than the verified one unless a check has passed since it
        // was written. It is used only if it is ahead; the first check after it covers the
        // squarings before it was saved too, as d is the product of every block's x since the
        // start, so a bad one is caught and the test goes back to the verified state.
        if (o.checkpoint_unverified) {
            ux = calloc(nl, 8);
            ud = calloc(nl, 8);
            if (ckpt_read(o.checkpoint_unverified, k, n, base, &ui, &uL, ux, ud, nl) || ui <= vi) ui = 0;
            else {
                res->resumed = 1;
                if (uL > L) L = uL;
            }
        }
        if (L > kite_fft_len(s.ctx)) {
                kite_opts fo = o.fft;
                fo.fft_len = L;
                res->careful_ops += s.careful;
        if (s.ctx) res->sum_errors += kite_sum_errors(s.ctx);
                state_free(&s);
                if (state_new(&s, k, n, fo, maxround)) {
                    rc = -1;
                    goto out;
                }
                power_start(&s, k, base, la);
        }
    }
    uint64_t full = n / B * B;  // Gerbicz-protected part
    double last_ck = now(), last_u = last_ck;
    int consecutive_fail = 0;
    res->resumed_at = ui ? ui : vi;
    uint32_t vp = 0, up = 0;  // next point not yet reported from vi, and from ui
    if (o.on_point) {
        while (vp < o.npoints && o.points[vp] < vi + (vi > 0)) vp++;
        if (vp < o.npoints && o.points[vp] == 0) {  // x_0 = base^k, checked by power_start
            kite_set_limbs(s.ctx, s.x, vx);
            pending_add(&pend, s.ctx, vp++, s.x);
            if (pending_flush(&pend, &o)) {
                rc = KITE_PRP_POINT_FAILED;
                goto out;
            }
        }
        // points up to an unverified save were reported when it was written
        up = vp;
        while (up < o.npoints && o.points[up] < ui + 1) up++;
    }
    for (;;) {
        // (re)start from the verified state, or the first time from an unverified one if ahead
        int from_u = ui > vi;
        kite_set_limbs(s.ctx, s.x, from_u ? ux : vx);
        kite_set_limbs(s.ctx, s.d, from_u ? ud : vd);
        kite_reset_roundoff(s.ctx);
        uint64_t start = from_u ? ui : vi, i = start;
        uint32_t pi = from_u ? up : vp;
        ui = 0;  // used once: a failure below goes back to the verified state
        int failed = 0, round_fail = 0;
        pending_clear(&pend);
        if (o.abort_flag && *o.abort_flag) {
            if (o.checkpoint) ckpt_write(o.checkpoint, k, n, base, vi, kite_fft_len(s.ctx), vx, vd, nl);
            rc = KITE_PRP_ABORTED;
            goto out;
        }
        uint32_t careful0 = s.careful;
        // run up to M blocks, keep d before the latest update in t
        uint64_t check_at = start + (uint64_t)B * M;
        if (check_at > full) check_at = full;
        while (i < check_at) {
            // A round is B*M squarings, minutes at n in the millions, so a stop request is read
            // after every block rather than only between rounds. A caller that sends SIGTERM waits
            // a bounded time and then kills, losing everything since the last checkpoint.
            if (o.abort_flag && *o.abort_flag) break;
            // A squaring over the round-off limit is redone carefully at once: the block is
            // replayed from its start (the squarings before it give the same values again) and
            // that squaring done as (x+r)(x-r)+r^2. Only if that is over too does the round fail.
            // Snapshots every SNAP squarings bound a replay to SNAP squarings (a 2 MB copy at 256K
            // costs ~0.1 ms against ~0.7 ms a squaring).
            enum { SNAP = 32 };
            uint64_t ib = i;
            uint32_t pib = pi, pcb = pend.count, jb = 0;
            for (uint32_t j = 0; j < B; j++) {
                if (j % SNAP == 0) {
                    kite_copy(s.ctx, s.b, s.x);
                    ib = i;
                    pib = pi;
                    pcb = pend.count;
                    jb = j;
                }
                square_at(&s, s.x, i);
                i++;
                if (o.inject_fault_at && i == o.inject_fault_at) {
                    kite_debug_corrupt(s.ctx, s.x);
                    o.inject_fault_at = 0;  // once
                }
                if (over(&s)) {
                    kite_reset_roundoff(s.ctx);
                    kite_copy(s.ctx, s.x, s.b);
                    pending_truncate(&pend, pcb);
                    i = ib;
                    pi = pib;
                    for (uint32_t jj = jb; jj < j; jj++) {
                        square_at(&s, s.x, i);
                        i++;
                        if (pi < o.npoints && o.points[pi] == i && o.on_point) pending_add(&pend, s.ctx, pi++, s.x);
                    }
                    careful_square(&s, s.x);
                    i++;
                    if (over(&s)) {
                        round_fail = 1;
                        break;
                    }
                }
                if (pi < o.npoints && o.points[pi] == i && o.on_point) pending_add(&pend, s.ctx, pi++, s.x);
            }
            if (round_fail) break;
            // t is kept at every block, not only the last of the round, so a stop part way through
            // can verify up to the block it reached instead of dropping the whole round.
            kite_copy(s.ctx, s.t, s.d);
            kite_mul(s.ctx, s.d, s.x);
            if (over(&s)) {
                kite_reset_roundoff(s.ctx);
                careful_mul(&s, s.d, s.t, s.x);
                if (over(&s)) {
                    round_fail = 1;
                    break;
                }
            }
            // Careful redos this frequent cost more than a longer FFT: move up a length here, at
            // the block boundary, with x, d and t carried over unverified. d is the product of
            // every block's x since the start, so the next check covers the squarings before
            // the move too, as for a resume from the unverified checkpoint.
            if (s.careful - careful0 > careful_limit) {
                uint32_t L = kite_fft_len(s.ctx);
                kite_opts fo = o.fft;
                fo.fft_len = kite_next_fft_len(L + L / 25);
                uint64_t *mx = malloc(nl * 8), *md = malloc(nl * 8), *mt = malloc(nl * 8);
                kite_get_limbs(s.ctx, s.x, mx);
                kite_get_limbs(s.ctx, s.d, md);
                kite_get_limbs(s.ctx, s.t, mt);
                res->careful_ops += s.careful;
        if (s.ctx) res->sum_errors += kite_sum_errors(s.ctx);
                state_free(&s);
                if (!fo.fft_len || state_new(&s, k, n, fo, maxround)) {
                    free(mx), free(md), free(mt);
                    rc = -2;
                    goto out;
                }
                power_start(&s, k, base, la);
                kite_set_limbs(s.ctx, s.x, mx);
                kite_set_limbs(s.ctx, s.d, md);
                kite_set_limbs(s.ctx, s.t, mt);
                free(mx), free(md), free(mt);
                careful0 = s.careful;
                res->roundoff_retries++;
            }
            if (o.progress) o.progress(o.progress_arg, i, n, kite_max_roundoff(s.ctx));
            // Unverified state every u_secs, for a test killed without the chance to stop. It
            // costs a file write, not B squarings. Points so far are reported first, so a resume
            // from it has them: a kill between the two leaves the older file, and the points are
            // redone from there. A failed check redoes them and reports them again.
            if (o.checkpoint_unverified && now() - last_u > u_secs && i < check_at) {
                if (pend.count && pending_flush(&pend, &o)) {
                    rc = KITE_PRP_POINT_FAILED;
                    goto out;
                }
                kite_get_limbs(s.ctx, s.x, la);
                kite_get_limbs(s.ctx, s.d, lb);
                ckpt_write(o.checkpoint_unverified, k, n, base, i, kite_fft_len(s.ctx), la, lb, nl);
                last_u = now();
            }
        }
        // Stopped part way through the round: verify up to the last block, then save and return
        // below. A round is about a quarter of the test at n in the millions (B = 1822, M = 455 at
        // n = 3.3M), so dropping it lost up to 25 minutes per test; the check costs B squarings.
        int stopping = o.abort_flag && *o.abort_flag;
        if (stopping && i == start) continue;  // nothing new to verify; saved at the top
        if (!round_fail && i > start) {
            // verify d == x0 * t^(2^B)
            if (square_n_checked(&s, s.t, B)) round_fail = 1;
            else {
                kite_copy(s.ctx, s.b, s.t);
                kite_mul(s.ctx, s.t, s.x0);
                if (over(&s)) {
                    kite_reset_roundoff(s.ctx);
                    careful_mul(&s, s.t, s.b, s.x0);
                    if (over(&s)) round_fail = 1;
                }
                if (!round_fail) failed = !nums_equal(s.ctx, s.t, s.d, la, lb);
            }
        }
        if (kite_max_roundoff(s.ctx) > res->maxerr) res->maxerr = kite_max_roundoff(s.ctx);
        // An unverified save from this round, or the one resumed from, may hold the error.
        if ((round_fail || failed) && o.checkpoint_unverified) remove(o.checkpoint_unverified);
        if (round_fail) {
            // redo from the verified state with the next larger FFT length
            uint32_t L = kite_fft_len(s.ctx);
            kite_opts fo = o.fft;
            fo.fft_len = kite_next_fft_len(L + L / 25);
            res->careful_ops += s.careful;
        if (s.ctx) res->sum_errors += kite_sum_errors(s.ctx);
            state_free(&s);
            if (!fo.fft_len || state_new(&s, k, n, fo, maxround)) {
                rc = -2;
                goto out;
            }
            power_start(&s, k, base, la);
            res->roundoff_retries++;
            continue;
        }
        if (failed) {
            res->gerbicz_errors++;
            if (++consecutive_fail > 5) {
                rc = -3;
                goto out;
            }
            continue;
        }
        consecutive_fail = 0;
        kite_get_limbs(s.ctx, s.x, vx);
        kite_get_limbs(s.ctx, s.d, vd);
        vi = i;
        vp = pi;
        if (pend.count && pending_flush(&pend, &o)) {
            rc = KITE_PRP_POINT_FAILED;
            goto out;
        }
        if (o.checkpoint && (now() - last_ck > ck_secs || vi == full || stopping)) {
            ckpt_write(o.checkpoint, k, n, base, vi, kite_fft_len(s.ctx), vx, vd, nl);
            last_ck = now();
        }
        if (stopping) {
            rc = KITE_PRP_ABORTED;
            goto out;
        }
        if (vi >= full) break;
    }
    // unprotected tail (< B squarings): compute twice and compare
    {
        uint64_t tail = n - full;
        for (int attempt = 0;; attempt++) {
            kite_set_limbs(s.ctx, s.x, vx);
            kite_set_limbs(s.ctx, s.t, vx);
            // stop at each point in the tail so it can be reported
            uint64_t i = full;
            uint32_t pi = vp;
            pending_clear(&pend);
            while (i < n) {
                uint64_t next = n;
                if (o.on_point && pi < o.npoints && o.points[pi] > i && o.points[pi] < next) next = o.points[pi];
                // checked like the blocks: two identical computations would repeat a round-off error
                if (square_n_checked(&s, s.x, next - i) || square_n_checked(&s, s.t, next - i)) {
                    uint32_t L = kite_fft_len(s.ctx);
                    kite_opts fo = o.fft;
                    fo.fft_len = kite_next_fft_len(L + L / 25);
                    res->careful_ops += s.careful;
        if (s.ctx) res->sum_errors += kite_sum_errors(s.ctx);
                    state_free(&s);
                    if (!fo.fft_len || state_new(&s, k, n, fo, maxround)) {
                        rc = -2;
                        goto out;
                    }
                    power_start(&s, k, base, la);
                    res->roundoff_retries++;
                    kite_set_limbs(s.ctx, s.x, vx);
                    kite_set_limbs(s.ctx, s.t, vx);
                    i = full;
                    pi = vp;
                    pending_clear(&pend);
                    continue;
                }
                i = next;
                if (o.on_point && pi < o.npoints && o.points[pi] == i) pending_add(&pend, s.ctx, pi++, s.x);
            }
            if (nums_equal(s.ctx, s.x, s.t, la, lb)) {
                if (pend.count && pending_flush(&pend, &o)) {
                    rc = KITE_PRP_POINT_FAILED;
                    goto out;
                }
                break;
            }
            if (attempt > 5) {
                rc = -3;
                goto out;
            }
            res->gerbicz_errors++;
        }
    }
    // la = x_n; PRP iff x_n == base^2; residue = x_n / base^2
    {
        int is_prp = la[0] == (uint64_t)base * base;
        for (size_t j = 1; j < nl; j++) is_prp &= la[j] == 0;
        uint64_t *Nl = calloc(nl, 8);
        uint64_t sh = n % 64, lw = n / 64;
        Nl[lw] = k << sh;
        if (sh && lw + 1 < nl) Nl[lw + 1] = k >> (64 - sh);
        for (size_t j = 0; j < nl; j++) {
            if (Nl[j]--) break;
        }
        if (kite_residue_nm1(la, Nl, nl, base)) rc = -4;
        free(Nl);
        res->res64 = la[0];
        res->is_prp = is_prp;
    }
    res->fft_len = kite_fft_len(s.ctx);
    if (kite_max_roundoff(s.ctx) > res->maxerr) res->maxerr = kite_max_roundoff(s.ctx);
out:
    res->careful_ops += s.careful;
        if (s.ctx) res->sum_errors += kite_sum_errors(s.ctx);
    res->seconds = now() - t_start;
    pending_clear(&pend);
    free(pend.index);
    free(pend.limbs);
    state_free(&s);
    free(la);
    free(lb);
    free(vx);
    free(vd);
    free(ux);
    free(ud);
    return rc;
}
