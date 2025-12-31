// numa_bw_lat_v3.c
// Build: gcc -O2 -march=native -pthread -o numa_bw_lat_v3 numa_bw_lat_v3.c
//
// Works for NUMA node count >= 2 (including your 4-node layout).
// - Pins CPU via sched_setaffinity (logical CPU id)
// - Binds memory via mbind(MPOL_BIND) to a specific NUMA node
// - BW: optional multi-thread streaming (threads pinned via --cpulist)
// - LAT: randomized pointer-chasing (defeats HW prefetch), default stride=4096

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef MPOL_BIND
#define MPOL_DEFAULT     0
#define MPOL_PREFERRED   1
#define MPOL_BIND        2
#define MPOL_INTERLEAVE  3
#endif

static long sys_mbind(void *addr, unsigned long len, int mode,
                      const unsigned long *nodemask, unsigned long maxnode,
                      unsigned flags) {
#ifdef __NR_mbind
    return syscall(__NR_mbind, addr, len, mode, nodemask, maxnode, flags);
#else
    errno = ENOSYS;
    return -1;
#endif
}

static uint64_t nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "sched_setaffinity(cpu=%d) failed: %s\n", cpu, strerror(errno));
        exit(2);
    }
}

static void touch_pages(uint8_t *buf, size_t len) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < len; i += page) buf[i] = (uint8_t)i;
    if (len) buf[len - 1] ^= 1;
}

static inline uint64_t rng_next(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s --cpu <id> --memnode <n> [--size-mb <MB>] [--iters <N>] [--stride <bytes>]\n"
        "          [--threads <T>] [--cpulist <c0,c1,...>] [--mode <bw|lat|both>]\n"
        "  --cpu       logical CPU id to pin main thread\n"
        "  --memnode   NUMA node id to bind allocation to\n"
        "  --size-mb   allocation size in MB (default 1024)\n"
        "  --iters     bandwidth loop iterations (default 50)\n"
        "  --stride    latency stride bytes (default 4096)\n"
        "  --threads   bandwidth threads (default 1)\n"
        "  --cpulist   CPU list for bw threads, e.g. \"12,13,22,23\" (len>=threads)\n"
        "  --mode      bw / lat / both (default both)\n",
        p);
}

static int parse_cpulist(const char *s, int **out, int *out_n) {
    if (!s || !*s) { *out = NULL; *out_n = 0; return 0; }
    int cap = 16, n = 0;
    int *arr = (int*)malloc(sizeof(int)*cap);
    if (!arr) return -1;

    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) { free(arr); return -1; }
        if (n == cap) { cap *= 2; arr = (int*)realloc(arr, sizeof(int)*cap); if (!arr) return -1; }
        arr[n++] = (int)v;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') p++;
    }
    *out = arr; *out_n = n;
    return 0;
}

static unsigned long *make_nodemask(int node, unsigned long maxnode_bits, unsigned long *mask_words_out) {
    const unsigned long bits_per_word = 8ul * (unsigned long)sizeof(unsigned long);
    const unsigned long words = (maxnode_bits + bits_per_word - 1) / bits_per_word;
    unsigned long *mask = (unsigned long*)calloc(words, sizeof(unsigned long));
    if (!mask) return NULL;
    unsigned long idx = (unsigned long)node / bits_per_word;
    unsigned long off = (unsigned long)node % bits_per_word;
    if (idx < words) mask[idx] |= (1ul << off);
    *mask_words_out = words;
    return mask;
}

// ---------------- bandwidth (optional multi-thread) ----------------

typedef struct {
    uint8_t *buf;
    size_t len;
    int iters;
    int cpu;
    volatile uint64_t sink;
} bw_task_t;

static void *bw_worker(void *arg) {
    bw_task_t *t = (bw_task_t*)arg;
    pin_cpu(t->cpu);

    volatile uint64_t sink = 0;
    for (int it = 0; it < t->iters; it++) {
        for (size_t i = 0; i < t->len; i += 64) {
            uint64_t *p = (uint64_t *)(t->buf + i);
            uint64_t v = p[0];
            p[0] = v ^ 0xA5A5A5A5A5A5A5A5ull;
            sink += v;
        }
    }
    t->sink = sink;
    return NULL;
}

static double run_bw_mt(uint8_t *buf, size_t len, int iters,
                        int threads, int cpu_fallback, int *cpus, int ncpus) {
    if (threads < 1) threads = 1;

    pthread_t *ths = (pthread_t*)malloc(sizeof(pthread_t)*threads);
    bw_task_t *tasks = (bw_task_t*)calloc((size_t)threads, sizeof(bw_task_t));
    if (!ths || !tasks) { fprintf(stderr, "alloc threads failed\n"); exit(2); }

    size_t chunk = len / (size_t)threads;
    chunk = (chunk / 64) * 64;
    if (chunk < 64) chunk = 64;

    uint64_t t0 = nsec_now();
    for (int i = 0; i < threads; i++) {
        size_t off = (size_t)i * chunk;
        size_t clen = (i == threads - 1) ? (len - off) : chunk;
        if (off >= len) { off = 0; clen = len; }

        tasks[i].buf = buf + off;
        tasks[i].len = clen;
        tasks[i].iters = iters;
        tasks[i].cpu = (cpus && ncpus >= threads) ? cpus[i] : cpu_fallback;

        pthread_create(&ths[i], NULL, bw_worker, &tasks[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(ths[i], NULL);
    uint64_t t1 = nsec_now();

    const double lines = (double)((len + 63) / 64);
    const double bytes = lines * 64.0 * (double)iters;
    const double sec = (double)(t1 - t0) / 1e9;

    volatile uint64_t sum = 0;
    for (int i = 0; i < threads; i++) sum += tasks[i].sink;
    if (sum == 0xdeadbeef) fprintf(stderr, "sum=%" PRIu64 "\n", (uint64_t)sum);

    free(ths);
    free(tasks);
    return bytes / sec;
}

// ---------------- latency (random pointer chasing) ----------------

static double run_lat_random(uint8_t *buf, size_t len, size_t stride) {
    if (stride < sizeof(uint32_t)) stride = sizeof(uint32_t);
    size_t n = len / stride;
    if (n < 2) n = 2;

    uint32_t *perm = (uint32_t*)malloc(sizeof(uint32_t) * n);
    if (!perm) { fprintf(stderr, "perm alloc failed\n"); exit(2); }
    for (size_t i = 0; i < n; i++) perm[i] = (uint32_t)i;

    uint64_t seed = (uint64_t)nsec_now() ^ (uint64_t)getpid();
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)(rng_next(&seed) % (i + 1));
        uint32_t tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }

    for (size_t i = 0; i < n; i++) {
        uint32_t cur = perm[i];
        uint32_t nxt = perm[(i + 1) % n];
        *(uint32_t *)(buf + (size_t)cur * stride) = nxt;
    }

    uint32_t idx = perm[0];
    for (size_t i = 0; i < n; i++) idx = *(uint32_t *)(buf + (size_t)idx * stride);

    const size_t steps = n * 64;
    uint64_t t0 = nsec_now();
    for (size_t i = 0; i < steps; i++) idx = *(uint32_t *)(buf + (size_t)idx * stride);
    uint64_t t1 = nsec_now();

    if (idx == 0xFFFFFFFFu) fprintf(stderr, "idx=%u\n", idx);
    free(perm);
    return (double)(t1 - t0) / (double)steps;
}

int main(int argc, char **argv) {
    int cpu = -1;
    int memnode = -1;
    size_t size_mb = 1024;
    int iters = 50;
    size_t stride = 4096;
    int threads = 1;
    char *cpulist_str = NULL;
    enum { MODE_BOTH, MODE_BW, MODE_LAT } mode = MODE_BOTH;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--memnode") && i + 1 < argc) memnode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size-mb") && i + 1 < argc) size_mb = (size_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc) stride = (size_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpulist") && i + 1 < argc) cpulist_str = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "bw")) mode = MODE_BW;
            else if (!strcmp(m, "lat")) mode = MODE_LAT;
            else mode = MODE_BOTH;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (cpu < 0 || memnode < 0) { usage(argv[0]); return 1; }

    pin_cpu(cpu);

    size_t len = size_mb * 1024ull * 1024ull;
    uint8_t *buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { fprintf(stderr, "mmap failed: %s\n", strerror(errno)); return 2; }

    const unsigned long maxnode_bits = 1024;
    unsigned long mask_words = 0;
    unsigned long *mask = make_nodemask(memnode, maxnode_bits, &mask_words);
    if (!mask) { fprintf(stderr, "nodemask alloc failed\n"); return 2; }

    if (sys_mbind(buf, (unsigned long)len, MPOL_BIND, mask, maxnode_bits, 0) != 0) {
        fprintf(stderr, "mbind(node=%d) failed: %s\n", memnode, strerror(errno));
        return 3;
    }

    touch_pages(buf, len);

    int *cpus = NULL, ncpus = 0;
    if (cpulist_str) {
        if (parse_cpulist(cpulist_str, &cpus, &ncpus) != 0) {
            fprintf(stderr, "Failed to parse --cpulist\n");
            return 2;
        }
    }

    printf("cpu=%d memnode=%d size_mb=%zu iters=%d stride=%zu threads=%d\n",
           cpu, memnode, size_mb, iters, stride, threads);

    if (mode == MODE_BW || mode == MODE_BOTH) {
        double bps = run_bw_mt(buf, len, iters, threads, cpu, cpus, ncpus);
        printf("bw_bytes_per_sec=%.0f bw_GiB_per_sec=%.3f\n", bps, bps / (1024.0*1024.0*1024.0));
    }
    if (mode == MODE_LAT || mode == MODE_BOTH) {
        double ns = run_lat_random(buf, len, stride);
        printf("lat_ns_per_load=%.2f\n", ns);
    }

    free(mask);
    free(cpus);
    munmap(buf, len);
    return 0;
}
