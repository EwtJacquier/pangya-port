#ifndef __EMSCRIPTEN__
// Sampling profiler (PANGYA_PROFILE=1): SIGPROF records the interrupted instruction pointer;
// at exit the hottest functions are printed (names via dladdr; needs -rdynamic).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#define MAX_SAMPLES (1 << 20)
static void *samples[MAX_SAMPLES];
static volatile int nsamples;

static void on_prof(int sig, siginfo_t *si, void *uc_) {
	(void)sig;
	(void)si;
	ucontext_t *uc = uc_;
	if (nsamples < MAX_SAMPLES) samples[nsamples++] = (void *)uc->uc_mcontext.gregs[REG_RIP];
}

typedef struct {
	const char *name;
	int count;
} Hit;

static int by_count(const void *a, const void *b) { return ((const Hit *)b)->count - ((const Hit *)a)->count; }

static void report(void) {
	Hit *hits = calloc((size_t)nsamples + 1, sizeof(Hit));
	int nh = 0;
	for (int i = 0; i < nsamples; i++) {
		Dl_info info;
		const char *name = dladdr(samples[i], &info) && info.dli_sname ? info.dli_sname : (info.dli_fname ? info.dli_fname : "?");
		int j;
		for (j = 0; j < nh; j++) {
			if (hits[j].name == name || !strcmp(hits[j].name, name)) break;
		}
		if (j == nh) hits[nh++] = (Hit){name, 0};
		hits[j].count++;
	}
	qsort(hits, (size_t)nh, sizeof(Hit), by_count);
	// Addresses without a dynamic symbol (static functions): offsets for addr2line.
	FILE *raw = fopen("build/profile_unnamed.txt", "w");
	for (int i = 0; raw && i < nsamples; i++) {
		Dl_info info;
		if (dladdr(samples[i], &info) && !info.dli_sname && info.dli_fname && strstr(info.dli_fname, "pangya"))
			fprintf(raw, "%lx\n", (unsigned long)((char *)samples[i] - (char *)info.dli_fbase));
	}
	if (raw) fclose(raw);
	fprintf(stderr, "PROFILE: %d samples\n", nsamples);
	for (int i = 0; i < nh && i < 30; i++) {
		fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * hits[i].count / nsamples, hits[i].name);
	}
}

__attribute__((constructor)) static void profile_init(void) {
	if (!getenv("PANGYA_PROFILE")) return;
	struct sigaction sa = {0};
	sa.sa_sigaction = on_prof;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &sa, NULL);
	struct itimerval it = {{0, 2000}, {0, 2000}};
	setitimer(ITIMER_PROF, &it, NULL);
	atexit(report);
}
#endif
