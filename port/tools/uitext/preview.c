// Offline renderer for reviewing tools/uitext output: runs the runtime's own atlas text code
// (runtime/text.c) on a decoded atlas.  preview IN.rgba W H K OUT.rgba  (env: PANGYA_UI_TEXT)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../../runtime/rt.h"

uint8_t *rt_mem;
void rt_log(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
uint64_t rt_now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int ge_tex_filter(void) { return 0; }  // (ge.c) the game's own filtering

#define UITEXT_CPU_ONLY  // no GL here: the CPU drawing (the reference)
#include "../../runtime/text.c"

int main(int argc, char **argv) {
	if (argc != 6) return fprintf(stderr, "usage: preview IN.rgba W H K OUT.rgba\n"), 2;
	int w = atoi(argv[2]), h = atoi(argv[3]), k = atoi(argv[4]);
	uint32_t *px = malloc(sizeof *px * (size_t)w * h);
	FILE *f = fopen(argv[1], "rb");
	if (!f || fread(px, 4, (size_t)w * h, f) != (size_t)w * h) return fprintf(stderr, "read %s\n", argv[1]), 1;
	fclose(f);
	const uint32_t *out = uitext_hires(px, w, h, k);
	if (!out) return fprintf(stderr, "atlas not in the spec\n"), 1;
	f = fopen(argv[5], "wb");
	fwrite(out, 4, (size_t)w * k * h * k, f);
	fclose(f);
	return 0;
}
