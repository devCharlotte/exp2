// wl.c
// usage: ./wl best|worst <size_mb> <hold_sec>
// - SIGUSR1을 받은 "그 순간"에 madvise(MERGEABLE) 호출
// - hold_sec 동안 read-touch만 수행해 PSS 측정이 안정적으로 나오도록 함
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <stdint.h>
#include <errno.h>

static volatile sig_atomic_t armed = 0;
static void on_sigusr1(int sig) { (void)sig; armed = 1; }

static void touch_pages_write(char *p, size_t sz) {
    const size_t step = 4096; // 4KB
    for (size_t off = 0; off < sz; off += step) p[off] = (char)0xAA;
}

static void touch_pages_read(char *p, size_t sz) {
    volatile unsigned long sink = 0;
    const size_t step = 4096;
    for (size_t off = 0; off < sz; off += step) sink += (unsigned long)p[off];
    (void)sink;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s best|worst size_mb hold_sec\n", argv[0]);
        return 2;
    }
    int best_mode = (strcmp(argv[1], "best") == 0);
    size_t size_mb = (size_t)strtoull(argv[2], NULL, 10);
    int hold_sec = atoi(argv[3]);
    if (size_mb == 0 || hold_sec <= 0) { fprintf(stderr, "invalid args\n"); return 2; }

    size_t sz = size_mb * 1024ULL * 1024ULL;
    char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }

    // 패턴 채우기
    if (best_mode) {
        // 실험 태그(64bit)를 반복해 채워 외부 프로세스와의 우연 병합을 차단
        const char *tag_env = getenv("KSM_TAG");   // 16진 16자리 권장
        uint64_t tag = tag_env ? strtoull(tag_env, NULL, 16)
                               : (0x5A5AA5A5ULL ^ (uint64_t)getpid());
        for (size_t off = 0; off + 8 <= sz; off += 8)
            *(uint64_t*)(p + off) = tag; // x86에선 언얼라인드 허용
    } else {
        // worst: 프로세스마다 다른 바이트로 페이지 시작만 다르게
        unsigned seed = (unsigned)getpid();
        for (size_t off = 0; off < sz; off += 4096)
            p[off] = (char)((seed + (unsigned)(off / 4096)) & 0xFF);
    }

    // 실제 메모리에 올림
    touch_pages_write(p, sz);

    // 시그널 핸들러
    struct sigaction sa = {0};
    sa.sa_handler = on_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);

    fprintf(stderr, "READY pid=%d size_mb=%zu mode=%s\n",
            getpid(), size_mb, best_mode ? "best" : "worst");
    fflush(stderr);

    // SIGUSR1 대기(최대 120s)
    for (int i = 0; i < 1200 && !armed; i++) usleep(100000);
    if (!armed) { fprintf(stderr, "timeout before armed; exiting\n"); return 1; }

    // 정확한 시작 시점에 병합 후보 표시
    if (madvise(p, sz, MADV_MERGEABLE) != 0)
        fprintf(stderr, "madvise(MERGEABLE) failed: %s\n", strerror(errno));
    else
        fprintf(stderr, "madvise(MERGEABLE) done pid=%d\n", getpid());

    // hold_sec 동안 1초마다 read-touch (병합 유지 + PSS 안정화)
    for (int sec = 0; sec < hold_sec; sec++) {
        touch_pages_read(p, sz);
        sleep(1);
    }
    return 0;
}
