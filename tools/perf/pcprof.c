// pcprof.c: a sampling profiler for machines where perf is not allowed (kernel.perf_event_paranoid above 1).
// Build: cc -O2 -shared -fPIC -o pcprof.so pcprof.c. Use: LD_PRELOAD=pcprof.so PCPROF_OUT=file program (pcprof.sh).
// A timer signals the main thread `PCPROF_HZ` times a second (default 10000). Each sample is the pc and the link register.
// The file has the process id (P), the executable mappings (M) and one line for each sample.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PCPROF_MAX (1u << 23)
static unsigned long* pcs;
static volatile unsigned long count;
static timer_t timer;

static void onSample(int sig, siginfo_t* info, void* context)
{
    (void)sig;
    (void)info;
    ucontext_t* c = context;
    if (count + 1 < PCPROF_MAX) {
        pcs[count++] = c->uc_mcontext.pc;
        pcs[count++] = c->uc_mcontext.regs[30];
    }
}

__attribute__((constructor)) static void start(void)
{
    const char* hz = getenv("PCPROF_HZ");
    long rate = hz ? atol(hz) : 10000;
    pcs = malloc(PCPROF_MAX * sizeof(long));
    struct sigaction a;
    memset(&a, 0, sizeof a);
    a.sa_sigaction = onSample;
    a.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &a, 0);
    struct sigevent e;
    memset(&e, 0, sizeof e);
    e.sigev_notify = SIGEV_THREAD_ID;
    e.sigev_signo = SIGPROF;
    e._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    if (timer_create(CLOCK_MONOTONIC, &e, &timer))
        return;
    struct itimerspec t = {{0, 1000000000 / rate}, {0, 1000000000 / rate}};
    timer_settime(timer, 0, &t, 0);
}

__attribute__((destructor)) static void stop(void)
{
    struct itimerspec t = {{0, 0}, {0, 0}};
    timer_settime(timer, 0, &t, 0);
    const char* o = getenv("PCPROF_OUT");
    FILE* f = fopen(o ? o : "pcprof.out", "w");
    if (!f)
        return;
    fprintf(f, "P %d\n", (int)getpid());
    FILE* m = fopen("/proc/self/maps", "r");
    char l[512];
    while (m && fgets(l, sizeof l, m))
        if (strstr(l, "r-xp"))
            fprintf(f, "M %s", l);
    if (m)
        fclose(m);
    for (unsigned long i = 0; i + 1 < count; i += 2)
        fprintf(f, "%lx %lx\n", pcs[i], pcs[i + 1]);
    fclose(f);
}
