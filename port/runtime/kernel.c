// PSP kernel services used by the game: threads (as coroutines on one OS thread, see coro.h),
// scheduler, event flags, variable pools, callbacks, partition memory, modules, misc.
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>

#include "coro.h"

#include "ge.h"
#include "hle.h"
#include "rt.h"

// ---- error codes -------------------------------------------------------------
#define ERR_ILLEGAL_ARG 0x800200D2u
#define ERR_NO_MEMORY 0x800200D9u
#define ERR_ILLEGAL_PRIORITY 0x8002012Au
#define ERR_UNKNOWN_UID 0x800200CBu
#define ERR_UNKNOWN_THID 0x80020198u
#define ERR_NOT_DORMANT 0x800201A4u
#define ERR_DORMANT 0x800201A2u
#define ERR_WAIT_TIMEOUT 0x800201A8u
#define ERR_WAIT_DELETE 0x800201B5u
#define ERR_EVF_COND 0x800201AFu
#define ERR_EVF_MULTI 0x800201B0u
#define ERR_EVF_ILPAT 0x800201B1u
#define ERR_UNKNOWN_EVFID 0x800201AEu
#define ERR_UNKNOWN_VPLID 0x800201C3u
#define ERR_ILLEGAL_MEMBLOCK 0x800200C0u
#define ERR_UNKNOWN_CBID 0x800201A1u

// ---- kernel object table -------------------------------------------------------
enum ObjType { OBJ_NONE, OBJ_THREAD, OBJ_EVF, OBJ_VPL, OBJ_CALLBACK, OBJ_BLOCK, OBJ_MODULE };
#define MAX_OBJS 2048
#define UID_BASE 0x00100000u
static struct {
	int type;
	void *p;
} objs[MAX_OBJS];

static uint32_t obj_new(int type, void *p) {
	for (int i = 1; i < MAX_OBJS; i++) {
		if (objs[i].type == OBJ_NONE) {
			objs[i].type = type;
			objs[i].p = p;
			return UID_BASE + (uint32_t)i;
		}
	}
	rt_fatal("kernel object table full");
}
static void *obj_get(uint32_t uid, int type) {
	uint32_t i = uid - UID_BASE;
	if (uid < UID_BASE || i >= MAX_OBJS || objs[i].type != type) {
		return NULL;
	}
	return objs[i].p;
}
static void obj_free(uint32_t uid) {
	uint32_t i = uid - UID_BASE;
	if (i < MAX_OBJS) {
		objs[i].type = OBJ_NONE;
		objs[i].p = NULL;
	}
}

// ---- partition memory ----------------------------------------------------------
typedef struct Block {
	uint32_t addr, size, uid;
	char name[32];
	struct Block *next;  // sorted by address
} Block;
static Block *blocks;
static uint32_t heap_lo, heap_hi;

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// type: 0 low, 1 high, 2 at address, 3 low aligned, 4 high aligned (arg = alignment).
static Block *block_alloc(uint32_t size, int type, uint32_t arg, const char *name) {
	size = align_up(size ? size : 1, 0x100);
	uint32_t align = (type == 3 || type == 4) && arg > 0x100 ? arg : 0x100;
	uint32_t found = 0;
	bool ok = false;
	// Walk gaps between blocks.
	uint32_t prev_end = heap_lo;
	Block *b = blocks;
	for (;;) {
		uint32_t gap_end = b ? b->addr : heap_hi;
		uint32_t lo = align_up(prev_end, align);
		uint32_t hi = gap_end >= size ? ((gap_end - size) & ~(align - 1)) : 0;
		if (gap_end > prev_end && lo <= hi && hi >= prev_end) {
			if ((type == 0 || type == 3) && !ok) {  // low: first fit
				found = lo;
				ok = true;
			}
			else if (type == 1 || type == 4) {  // high: last fit wins
				found = hi;
				ok = true;
			}
			else if (type == 2 && arg >= prev_end && arg + size <= gap_end) {
				found = arg;
				ok = true;
			}
		}
		if (!b) {
			break;
		}
		prev_end = b->addr + b->size;
		b = b->next;
	}
	if (!ok) {
		return NULL;
	}
	Block *nb = calloc(1, sizeof(Block));
	nb->addr = found;
	nb->size = size;
	snprintf(nb->name, sizeof nb->name, "%s", name ? name : "");
	Block **pp = &blocks;
	while (*pp && (*pp)->addr < found) {
		pp = &(*pp)->next;
	}
	nb->next = *pp;
	*pp = nb;
	nb->uid = obj_new(OBJ_BLOCK, nb);
	return nb;
}

static void block_free(Block *blk) {
	for (Block **pp = &blocks; *pp; pp = &(*pp)->next) {
		if (*pp == blk) {
			*pp = blk->next;
			obj_free(blk->uid);
			free(blk);
			return;
		}
	}
}

uint32_t rt_alloc_user(uint32_t size, bool high, const char *what) {
	Block *b = block_alloc(size, high ? 1 : 0, 0, what);
	return b ? b->addr : 0;
}

void rt_free_user(uint32_t addr) {
	for (Block *b = blocks; b; b = b->next) {
		if (b->addr == addr) {
			block_free(b);
			return;
		}
	}
}

uint32_t rt_user_free_total(void) {
	uint32_t used = 0;
	for (Block *b = blocks; b; b = b->next) {
		used += b->size;
	}
	return heap_hi - heap_lo - used;
}

// ---- threads -------------------------------------------------------------------
enum { TS_RUNNING = 1, TS_READY = 2, TS_WAIT = 4, TS_SUSPEND = 8, TS_DORMANT = 16, TS_DEAD = 32 };
enum { WT_NONE, WT_DELAY, WT_VBLANK, WT_EVF, WT_THREADEND, WT_VPL, WT_TIME };

struct Thread {
	uint32_t uid;
	char name[32];
	CPU cpu;
	Coro co;
	uint32_t entry, init_prio, prio, attr, stack_addr, stack_size;
	int status;
	int64_t ready_seq;
	int wait_type;
	void *wait_obj;
	uint64_t wake_us;  // 0 = no timeout
	uint32_t timeout_ptr;
	uint32_t wait_bits, wait_mode, wait_out;  // event flag: bits, mode, out ptr; vpl: size, -, out ptr
	int32_t wait_result;
	uint32_t exit_status;
	uint64_t vblank_target;
};

#define MAX_THREADS 64
static Thread threads[MAX_THREADS];
static Thread *cur;

// Debug (GE capture): every live thread with its VFPU registers.
void rt_dump_threads(FILE *f) {
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *t = &threads[i];
		if (!t->uid || t->status == TS_DEAD) continue;
		fprintf(f, "THREAD %s uid=%08x status=%d attr=%08x prio=%u pc~ra=%08x vctrl=%05x/%05x/%03x\nVREG %s", t->name, t->uid,
		        t->status, t->attr, t->prio, t->cpu.r[31], t->cpu.vfpu_ctrl[0], t->cpu.vfpu_ctrl[1], t->cpu.vfpu_ctrl[2], t->name);
		for (int k = 0; k < 128; k++) {
			uint32_t u;
			memcpy(&u, &t->cpu.v[k], 4);
			fprintf(f, " %08x", u);
		}
		fprintf(f, "\n");
	}
}
static Coro sched_co;
static int64_t seq_back, seq_front;
static uint64_t vblanks, next_vblank_us;
static uint64_t next_timer_us = UINT64_MAX;  // earliest pending wake-up of a waiting thread

Thread *rt_current_thread(void) { return cur; }
const char *rt_thread_name(Thread *t) { return t->name; }
uint64_t rt_vblank_count(void) { return vblanks; }

static Thread *thread_get(uint32_t uid) {
	if (uid == 0) {
		return cur;
	}
	return obj_get(uid, OBJ_THREAD);
}

static void make_ready(Thread *t, bool front) {
	t->status = (t->status & TS_SUSPEND) ? (TS_SUSPEND) : TS_READY;
	t->ready_seq = front ? --seq_front : ++seq_back;
}

static void wake(Thread *t, int32_t result) {
	t->wait_type = WT_NONE;
	t->wait_obj = NULL;
	t->wait_result = result;
	t->wake_us = 0;
	if (t->status & TS_SUSPEND) {
		t->status = TS_SUSPEND;
	}
	else {
		make_ready(t, false);
	}
}

static void switch_to_scheduler(void) {
	Thread *t = cur;
	coro_switch(&t->co, &sched_co);
}

// Block the current thread; returns the wait result.
static int32_t block_current(int wait_type, void *obj, uint64_t wake_us) {
	Thread *t = cur;
	t->status = TS_WAIT;
	t->wait_type = wait_type;
	t->wait_obj = obj;
	t->wake_us = wake_us;
	t->wait_result = 0;
	if (wake_us && wake_us < next_timer_us) {
		next_timer_us = wake_us;
	}
	switch_to_scheduler();
	return t->wait_result;
}

static uint64_t timeout_deadline(uint32_t timeout_ptr) {
	if (!timeout_ptr) {
		return 0;
	}
	uint32_t us = M32(timeout_ptr);
	return rt_now_us() + (us ? us : 1);
}

void rt_wait_until(uint64_t wake_us) { block_current(WT_DELAY, NULL, wake_us ? wake_us : 1); }

void rt_wait_vblank(void) {
	cur->vblank_target = vblanks + 1;
	block_current(WT_VBLANK, NULL, 0);
}

// Runs fn(arg) on the scheduler's own stack and returns. In the browser the asynchronous
// JavaScript waits (Asyncify) all happen there, so a game thread's fiber only ever unwinds for
// fiber switches.
static void (*main_call_fn)(void *);
static void *main_call_arg;
void rt_call_on_main(void (*fn)(void *), void *arg) {
	if (!cur) {
		fn(arg);
		return;
	}
	main_call_fn = fn;
	main_call_arg = arg;
	coro_switch(&cur->co, &sched_co);
}

void rt_yield(void) {
	make_ready(cur, false);
	switch_to_scheduler();
}

static Thread *best_ready(void) {
	Thread *best = NULL;
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *t = &threads[i];
		if (t->uid && t->status == TS_READY &&
		    (!best || t->prio < best->prio || (t->prio == best->prio && t->ready_seq < best->ready_seq))) {
			best = t;
		}
	}
	return best;
}

// Called before every HLE call: emulates the timer/vblank interrupts that would preempt the
// running thread on a real PSP, so a woken higher-priority thread gets to run.
void rt_hle_poll(void) {
	if (!cur) {
		return;
	}
	uint64_t now = rt_now_us();
	if (now >= next_vblank_us || now >= next_timer_us) {
		make_ready(cur, true);  // stays first among its priority level
		switch_to_scheduler();
	}
}

void rt_resched(void) {
	Thread *b = best_ready();
	if (b && cur && b->prio < cur->prio) {
		make_ready(cur, true);  // preempted: stays at the head of its priority level
		switch_to_scheduler();
	}
}

static void thread_exit(Thread *t, uint32_t status);

static void thread_trampoline(void *arg) {
	Thread *t = arg;
	RecompFunc f = rt_lookup(t->entry);
	if (!f) {
		rt_fatal("thread %s: entry %08x is not a known function", t->name, t->entry);
	}
	f(&t->cpu);
	thread_exit(t, t->cpu.r[2]);
}

static void thread_exit(Thread *t, uint32_t status) {
	TRACE("thread %s exits with %08x", t->name, status);
	t->status = TS_DORMANT;
	t->exit_status = status;
	t->wait_type = WT_NONE;
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (w->uid && w->status == TS_WAIT && w->wait_type == WT_THREADEND && w->wait_obj == t) {
			wake(w, (int32_t)status);
		}
	}
	switch_to_scheduler();
	rt_fatal("dormant thread %s resumed", t->name);
}

static Thread *thread_create(const char *name, uint32_t entry, uint32_t prio, uint32_t stack_size, uint32_t attr) {
	Thread *t = NULL;
	for (int i = 0; i < MAX_THREADS; i++) {
		if (!threads[i].uid) {
			t = &threads[i];
			break;
		}
	}
	if (!t) {
		return NULL;
	}
	Coro co = t->co;  // keep the host stack allocations for reuse
	memset(t, 0, sizeof *t);
	t->co = co;
	snprintf(t->name, sizeof t->name, "%s", name);
	t->entry = entry;
	t->init_prio = t->prio = prio;
	t->attr = attr;
	t->stack_size = align_up(stack_size < 0x200 ? 0x200 : stack_size, 0x100);
	t->stack_addr = rt_alloc_user(t->stack_size, true, name);
	if (!t->stack_addr) {
		return NULL;
	}
	if (!(attr & 0x00100000)) {  // PSP_THREAD_ATTR_NO_FILLSTACK
		memset(MP(t->stack_addr), 0xFF, t->stack_size);
	}
	M32(t->stack_addr) = 0;
	t->status = TS_DORMANT;
	t->uid = obj_new(OBJ_THREAD, t);
	M32(t->stack_addr) = t->uid;  // the kernel stores the thread id at the stack bottom
	return t;
}

static void thread_start(Thread *t, uint32_t arg_len, uint32_t argp) {
	memset(&t->cpu, 0, sizeof t->cpu);
	t->cpu.vfpu_ctrl[0] = t->cpu.vfpu_ctrl[1] = 0xE4;  // VFPU S/T prefixes: identity swizzle
	uint32_t top = t->stack_addr + t->stack_size;
	uint32_t sp = top - 0x100;
	t->cpu.r[26] = sp;  // k0: thread context area
	if (arg_len && argp) {
		sp -= align_up(arg_len, 16);
		memmove(MP(sp), MP(argp), arg_len);
		t->cpu.r[5] = sp;
	}
	t->cpu.r[4] = arg_len;
	t->cpu.r[29] = sp - 0x40;
	t->cpu.r[31] = 0;
	t->prio = t->init_prio;
	coro_init(&t->co, thread_trampoline, t);
	make_ready(t, false);
}

void kernel_create_main_thread(uint32_t entry, uint32_t prio, uint32_t stack_size, const char *argv0) {
	Thread *t = thread_create("user_main", entry, prio, stack_size, 0x80000000u);
	if (!t) {
		rt_fatal("cannot create main thread");
	}
	// Argument block: the boot path, copied by thread_start to the top of the stack.
	uint32_t len = (uint32_t)strlen(argv0) + 1;
	uint32_t tmp = rt_alloc_user(len, false, "argv");
	memcpy(MP(tmp), argv0, len);
	thread_start(t, len, tmp);
	rt_free_user(tmp);
	rt_log("main thread %08x entry %08x stack %08x+%x", t->uid, entry, t->stack_addr, t->stack_size);
}

// ---- event flags -------------------------------------------------------------
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t attr, init, pattern;
} EventFlag;

static bool evf_match(EventFlag *e, uint32_t bits, uint32_t mode) {
	return (mode & 1) ? (e->pattern & bits) != 0 : (e->pattern & bits) == bits;
}
static void evf_consume(EventFlag *e, uint32_t bits, uint32_t mode, uint32_t out) {
	if (out) {
		M32(out) = e->pattern;
	}
	if (mode & 0x10) {
		e->pattern = 0;  // CLEARALL
	}
	else if (mode & 0x20) {
		e->pattern &= ~bits;  // CLEAR
	}
}

static int evf_waiters(EventFlag *e) {
	int n = 0;
	for (int i = 0; i < MAX_THREADS; i++) {
		n += threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_EVF && threads[i].wait_obj == e;
	}
	return n;
}

static void evf_wake_waiters(EventFlag *e) {
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (w->uid && w->status == TS_WAIT && w->wait_type == WT_EVF && w->wait_obj == e &&
		    evf_match(e, w->wait_bits, w->wait_mode)) {
			evf_consume(e, w->wait_bits, w->wait_mode, w->wait_out);
			wake(w, 0);
		}
	}
}

// ---- variable pools ----------------------------------------------------------------
typedef struct VplAlloc {
	uint32_t addr, size;  // addr = block start (header), size includes 8-byte header
	struct VplAlloc *next;
} VplAlloc;
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t attr, base, size;
	VplAlloc *allocs;  // sorted
	Block *mem;
} Vpl;

static uint32_t vpl_try_alloc(Vpl *v, uint32_t size) {
	uint32_t need = align_up(size, 8) + 8;
	uint32_t prev = v->base;
	VplAlloc **pp = &v->allocs;
	for (;;) {
		uint32_t end = *pp ? (*pp)->addr : v->base + v->size;
		if (end >= prev && end - prev >= need) {
			VplAlloc *a = calloc(1, sizeof *a);
			a->addr = prev;
			a->size = need;
			a->next = *pp;
			*pp = a;
			M32(prev) = v->uid;
			M32(prev + 4) = need;
			return prev + 8;
		}
		if (!*pp) {
			return 0;
		}
		prev = (*pp)->addr + (*pp)->size;
		pp = &(*pp)->next;
	}
}

static uint32_t vpl_free_bytes(Vpl *v) {
	uint32_t used = 0;
	for (VplAlloc *a = v->allocs; a; a = a->next) {
		used += a->size;
	}
	return v->size - used;
}

static void vpl_wake_waiters(Vpl *v) {
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *w = &threads[i];
		if (w->uid && w->status == TS_WAIT && w->wait_type == WT_VPL && w->wait_obj == v) {
			uint32_t a = vpl_try_alloc(v, w->wait_bits);
			if (a) {
				M32(w->wait_out) = a;
				wake(w, 0);
			}
		}
	}
}

// ---- callbacks -------------------------------------------------------------------
typedef struct {
	uint32_t uid;
	char name[32];
	uint32_t func, arg;
	Thread *owner;
	int notify_count;
	uint32_t notify_arg;
} Callback;

// ---- scheduler ---------------------------------------------------------------------
void kernel_init(void) {
	heap_lo = align_up(rt_module_end, 0x100);
	heap_hi = PSP_RAM_END;
	next_vblank_us = VBLANK_US;
	// The loaded module occupies its own block so allocations never overlap it.
	(void)block_alloc(rt_module_end - USER_PARTITION_START, 2, USER_PARTITION_START, "module");
}

static void check_timers(uint64_t now) {
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *t = &threads[i];
		if (!t->uid || t->status != TS_WAIT) {
			continue;
		}
		if (t->wait_type == WT_VBLANK && vblanks >= t->vblank_target) {
			wake(t, 0);
		}
		else if (t->wake_us && now >= t->wake_us) {
			bool timeout = t->wait_type != WT_DELAY && t->wait_type != WT_TIME;
			if (timeout && t->timeout_ptr) {
				M32(t->timeout_ptr) = 0;
			}
			wake(t, timeout ? (int32_t)ERR_WAIT_TIMEOUT : 0);
		}
	}
}

extern volatile unsigned long rt_sched_heartbeat;

void kernel_run(void) {
	coro_init_main(&sched_co);
	for (;;) {
		rt_sched_heartbeat++;
		uint64_t now = rt_now_us();
		if (now >= next_vblank_us) {
			vblanks++;
			next_vblank_us += VBLANK_US;
			if (now > next_vblank_us + 4 * VBLANK_US) {
				next_vblank_us = now + VBLANK_US;  // we fell behind (long-running thread); resync
			}
			platform_vblank();
		}
		check_timers(now);
		next_timer_us = UINT64_MAX;
		for (int i = 0; i < MAX_THREADS; i++) {
			if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wake_us && threads[i].wake_us < next_timer_us) {
				next_timer_us = threads[i].wake_us;
			}
		}
		Thread *t = best_ready();
		if (!t) {
			uint64_t next = next_vblank_us;
			for (int i = 0; i < MAX_THREADS; i++) {
				if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wake_us && threads[i].wake_us < next) {
					next = threads[i].wake_us;
				}
			}
			now = rt_now_us();
			if (next > now) {
				platform_idle(next - now);
			}
			continue;
		}
		cur = t;
		t->status = TS_RUNNING;
		coro_switch(&sched_co, &t->co);
		while (main_call_fn) {  // the thread asked to run something here (rt_call_on_main)
			void (*fn)(void *) = main_call_fn;
			main_call_fn = NULL;
			fn(main_call_arg);
			coro_switch(&sched_co, &t->co);
		}
		cur = NULL;
	}
}

// Debug: one line per thread (called from the watchdog signal handler; uses fprintf on purpose,
// this is only for diagnosing hangs).
void kernel_dump_threads(void) {
	static const char *wt[] = {"-", "delay", "vblank", "evf", "thread-end", "vpl", "time"};
	for (int i = 0; i < MAX_THREADS; i++) {
		Thread *t = &threads[i];
		if (!t->uid) continue;
		fprintf(stderr, "  %-14s prio=%02x status=%2d wait=%-10s sp=%08x ra=%08x%s\n", t->name, t->prio, t->status,
		        t->status & TS_WAIT ? wt[t->wait_type] : "", t->cpu.r[29], t->cpu.r[31], t == cur ? "  <- running" : "");
	}
}

void rt_call_guest(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2) {
	CPU *c = &cur->cpu;
	CPU saved = *c;
	c->r[4] = a0;
	c->r[5] = a1;
	c->r[6] = a2;
	c->r[31] = 0;
	rt_call(c, func);
	*c = saved;
}

// =====================================================================================
// HLE: ThreadManForUser
// =====================================================================================

void hle_sceKernelCreateThread(CPU *c) {
	const char *name = rt_str(A0);
	Thread *t = thread_create(name, A1, A2, A3, A4);
	TRACE("sceKernelCreateThread(%s, entry=%08x, prio=%x, stack=%x, attr=%x) -> %08x", name, A1, A2, A3, A4,
	      t ? t->uid : 0);
	RET(t ? t->uid : ERR_NO_MEMORY);
}

void hle_sceKernelStartThread(CPU *c) {
	Thread *t = thread_get(A0);
	TRACE("sceKernelStartThread(%08x %s, len=%x, argp=%08x)", A0, t ? t->name : "?", A1, A2);
	if (!t) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	if (t->status != TS_DORMANT) {
		RET(ERR_NOT_DORMANT);
		return;
	}
	thread_start(t, A1, A2);
	RET(0);
	rt_resched();
}

void hle_sceKernelExitThread(CPU *c) { thread_exit(cur, A0); }

void hle_sceKernelDeleteThread(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t || t == cur) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	if (t->status != TS_DORMANT) {
		RET(ERR_NOT_DORMANT);
		return;
	}
	rt_free_user(t->stack_addr);
	obj_free(t->uid);
	t->uid = 0;
	RET(0);
}

void hle_sceKernelWaitThreadEnd(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	if (t->status == TS_DORMANT) {
		RET(t->exit_status);
		return;
	}
	cur->timeout_ptr = A1;
	RET(block_current(WT_THREADEND, t, timeout_deadline(A1)));
}
void hle_sceKernelWaitThreadEndCB(CPU *c) { hle_sceKernelWaitThreadEnd(c); }

void hle_sceKernelSuspendThread(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t || t == cur) {
		RET(ERR_ILLEGAL_ARG);
		return;
	}
	if (t->status & TS_DORMANT) {
		RET(ERR_DORMANT);
		return;
	}
	if (t->status & TS_SUSPEND) {
		RET(0x800201A1u - 0x0);  // already suspended (game logs and ignores)
		return;
	}
	t->status = (t->status == TS_WAIT) ? (TS_WAIT | TS_SUSPEND) : TS_SUSPEND;
	RET(0);
}

void hle_sceKernelResumeThread(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t || !(t->status & TS_SUSPEND)) {
		RET(0x800201A2u);
		return;
	}
	if (t->status == (TS_WAIT | TS_SUSPEND)) {
		t->status = TS_WAIT;
	}
	else {
		make_ready(t, false);
	}
	RET(0);
	rt_resched();
}

void hle_sceKernelGetThreadId(CPU *c) { RET(cur->uid); }
void hle_sceKernelGetThreadCurrentPriority(CPU *c) { RET(cur->prio); }
void hle_sceKernelChangeCurrentThreadAttr(CPU *c) {
	cur->attr = (cur->attr & ~A0) | A1;
	RET(0);
}

void hle_sceKernelReferThreadStatus(CPU *c) {
	Thread *t = thread_get(A0);
	if (!t) {
		RET(ERR_UNKNOWN_THID);
		return;
	}
	uint32_t p = A1;
	uint32_t size = M32(p);
	uint8_t info[0x68] = {0};
	uint32_t *w = (uint32_t *)info;
	w[0] = size;
	memcpy(info + 4, t->name, 32);
	w[9] = t->attr;
	w[10] = t->status == TS_RUNNING ? 1 : t->status;
	w[11] = t->entry;
	w[12] = t->stack_addr;
	w[13] = t->stack_size;
	w[14] = 0;  // gp
	w[15] = t->init_prio;
	w[16] = t->prio;
	w[17] = t->status == TS_WAIT ? (t->wait_type == WT_DELAY ? 2 : 1) : 0;
	w[20] = t->exit_status;
	memcpy(MP(p), info, size < sizeof info ? size : sizeof info);
	RET(0);
}

void hle_sceKernelDelayThread(CPU *c) {
	uint32_t us = A0 < 200 ? 200 : A0;  // the PSP rounds tiny delays up
	RET(0);
	rt_wait_until(rt_now_us() + us);
	RET(0);
}
void hle_sceKernelDelayThreadCB(CPU *c) { hle_sceKernelDelayThread(c); }

void hle_sceKernelGetSystemTimeLow(CPU *c) { RET((uint32_t)rt_now_us()); }
void hle_sceKernelGetSystemTimeWide(CPU *c) { RET64(rt_now_us()); }

// -- event flags
void hle_sceKernelCreateEventFlag(CPU *c) {
	EventFlag *e = calloc(1, sizeof *e);
	snprintf(e->name, sizeof e->name, "%s", rt_str(A0));
	e->attr = A1;
	e->init = e->pattern = A2;
	e->uid = obj_new(OBJ_EVF, e);
	TRACE("sceKernelCreateEventFlag(%s, attr=%x, init=%x) -> %08x", e->name, A1, A2, e->uid);
	RET(e->uid);
}

void hle_sceKernelDeleteEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	for (int i = 0; i < MAX_THREADS; i++) {
		if (threads[i].uid && threads[i].status == TS_WAIT && threads[i].wait_type == WT_EVF && threads[i].wait_obj == e) {
			wake(&threads[i], (int32_t)ERR_WAIT_DELETE);
		}
	}
	obj_free(e->uid);
	free(e);
	RET(0);
	rt_resched();
}

void hle_sceKernelSetEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	e->pattern |= A1;
	evf_wake_waiters(e);
	RET(0);
	rt_resched();
}

void hle_sceKernelClearEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	e->pattern &= A1;
	RET(0);
}

void hle_sceKernelPollEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	if (A1 == 0) {
		RET(ERR_EVF_ILPAT);
		return;
	}
	if (evf_match(e, A1, A2)) {
		evf_consume(e, A1, A2, A3);
		RET(0);
	}
	else {
		if (A3) {
			M32(A3) = e->pattern;
		}
		RET(ERR_EVF_COND);
	}
}

void hle_sceKernelWaitEventFlag(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	if (A1 == 0) {
		RET(ERR_EVF_ILPAT);
		return;
	}
	if (evf_match(e, A1, A2)) {
		evf_consume(e, A1, A2, A3);
		RET(0);
		return;
	}
	if (!(e->attr & 0x200) && evf_waiters(e) > 0) {  // PSP_EVENT_WAITMULTIPLE
		RET(ERR_EVF_MULTI);
		return;
	}
	cur->wait_bits = A1;
	cur->wait_mode = A2;
	cur->wait_out = A3;
	cur->timeout_ptr = A4;
	RET(block_current(WT_EVF, e, timeout_deadline(A4)));
}

void hle_sceKernelReferEventFlagStatus(CPU *c) {
	EventFlag *e = obj_get(A0, OBJ_EVF);
	if (!e) {
		RET(ERR_UNKNOWN_EVFID);
		return;
	}
	uint32_t p = A1;
	M32(p + 0) = 0x34;
	rt_write_cstr(p + 4, 32, e->name);
	M32(p + 36) = e->attr;
	M32(p + 40) = e->init;
	M32(p + 44) = e->pattern;
	M32(p + 48) = (uint32_t)evf_waiters(e);
	RET(0);
}

// -- variable pools
void hle_sceKernelCreateVpl(CPU *c) {
	uint32_t size = A3;
	Vpl *v = calloc(1, sizeof *v);
	snprintf(v->name, sizeof v->name, "%s", rt_str(A0));
	v->attr = A2;
	v->mem = block_alloc(size, (A2 & 0x4000) ? 1 : 0, 0, v->name);
	if (!v->mem) {
		free(v);
		RET(ERR_NO_MEMORY);
		return;
	}
	v->base = v->mem->addr;
	v->size = size;
	v->uid = obj_new(OBJ_VPL, v);
	TRACE("sceKernelCreateVpl(%s, part=%d, attr=%x, size=%x) -> %08x at %08x", v->name, A1, A2, size, v->uid, v->base);
	RET(v->uid);
}

void hle_sceKernelDeleteVpl(CPU *c) {
	Vpl *v = obj_get(A0, OBJ_VPL);
	if (!v) {
		RET(ERR_UNKNOWN_VPLID);
		return;
	}
	while (v->allocs) {
		VplAlloc *n = v->allocs->next;
		free(v->allocs);
		v->allocs = n;
	}
	block_free(v->mem);
	obj_free(v->uid);
	free(v);
	RET(0);
}

void hle_sceKernelAllocateVpl(CPU *c) {
	Vpl *v = obj_get(A0, OBJ_VPL);
	if (!v) {
		RET(ERR_UNKNOWN_VPLID);
		return;
	}
	uint32_t a = vpl_try_alloc(v, A1);
	if (a) {
		M32(A2) = a;
		RET(0);
		return;
	}
	TRACE("sceKernelAllocateVpl(%s, %x): pool full, waiting", v->name, A1);
	cur->wait_bits = A1;
	cur->wait_out = A2;
	cur->timeout_ptr = A3;
	RET(block_current(WT_VPL, v, timeout_deadline(A3)));
}

void hle_sceKernelFreeVpl(CPU *c) {
	Vpl *v = obj_get(A0, OBJ_VPL);
	if (!v) {
		RET(ERR_UNKNOWN_VPLID);
		return;
	}
	uint32_t hdr = A1 - 8;
	for (VplAlloc **pp = &v->allocs; *pp; pp = &(*pp)->next) {
		if ((*pp)->addr == hdr) {
			VplAlloc *a = *pp;
			*pp = a->next;
			free(a);
			vpl_wake_waiters(v);
			RET(0);
			rt_resched();
			return;
		}
	}
	RET(ERR_ILLEGAL_MEMBLOCK);
}

void hle_sceKernelReferVplStatus(CPU *c) {
	Vpl *v = obj_get(A0, OBJ_VPL);
	if (!v) {
		RET(ERR_UNKNOWN_VPLID);
		return;
	}
	uint32_t p = A1;
	M32(p) = 0x34;
	rt_write_cstr(p + 4, 32, v->name);
	M32(p + 36) = v->attr;
	M32(p + 40) = v->size;
	M32(p + 44) = vpl_free_bytes(v);
	M32(p + 48) = 0;
	RET(0);
}

// -- callbacks
void hle_sceKernelCreateCallback(CPU *c) {
	Callback *cb = calloc(1, sizeof *cb);
	snprintf(cb->name, sizeof cb->name, "%s", rt_str(A0));
	cb->func = A1;
	cb->arg = A2;
	cb->owner = cur;
	cb->uid = obj_new(OBJ_CALLBACK, cb);
	TRACE("sceKernelCreateCallback(%s, %08x, %08x) -> %08x", cb->name, A1, A2, cb->uid);
	RET(cb->uid);
}

void hle_sceKernelDeleteCallback(CPU *c) {
	Callback *cb = obj_get(A0, OBJ_CALLBACK);
	if (!cb) {
		RET(ERR_UNKNOWN_CBID);
		return;
	}
	obj_free(cb->uid);
	free(cb);
	RET(0);
}

static int pending_callbacks;

void hle_sceKernelCheckCallback(CPU *c) {
	if (!pending_callbacks) {
		// Games spin on this while another thread works; nothing can change before the next
		// timer or vblank, so sleep until then instead of burning the CPU.
		uint64_t next = next_vblank_us < next_timer_us ? next_vblank_us : next_timer_us;
		uint64_t now = rt_now_us();
		rt_wait_until(next > now + 100 ? next : now + 100);
		RET(0);
		return;
	}
	int ran = 0;
	for (int i = 1; i < MAX_OBJS; i++) {
		if (objs[i].type == OBJ_CALLBACK) {
			Callback *cb = objs[i].p;
			if (cb->owner == cur && cb->notify_count) {
				int n = cb->notify_count;
				cb->notify_count = 0;
				pending_callbacks--;
				rt_call_guest(cb->func, (uint32_t)n, cb->notify_arg, cb->arg);
				ran++;
			}
		}
	}
	RET(ran);
}

// =====================================================================================
// HLE: SysMemUserForUser, Kernel_Library, LoadExec, ModuleMgr, Stdio, Utils, Suspend...
// =====================================================================================

void hle_sceKernelAllocPartitionMemory(CPU *c) {
	const char *name = rt_str(A1);
	Block *b = block_alloc(A3, (int)A2, A4, name);
	TRACE("sceKernelAllocPartitionMemory(part=%d, %s, type=%d, size=%x, addr=%x) -> %08x", A0, name, A2, A3, A4,
	      b ? b->addr : 0);
	RET(b ? b->uid : ERR_NO_MEMORY);
}

void hle_sceKernelFreePartitionMemory(CPU *c) {
	Block *b = obj_get(A0, OBJ_BLOCK);
	if (!b) {
		RET(ERR_UNKNOWN_UID);
		return;
	}
	block_free(b);
	RET(0);
}

void hle_sceKernelGetBlockHeadAddr(CPU *c) {
	Block *b = obj_get(A0, OBJ_BLOCK);
	RET(b ? b->addr : 0);
}

void hle_sceKernelTotalFreeMemSize(CPU *c) { RET(rt_user_free_total()); }
void hle_sceKernelSetCompilerVersion(CPU *c) { RET(0); }
void hle_sceKernelSetCompiledSdkVersion395(CPU *c) { RET(0); }

// Minimal printf for guest varargs (integers and strings only), args in a1..a3, t0..t3.
static void guest_format(CPU *c, uint32_t fmt_addr, int first_reg, char *out, size_t cap) {
	static const int regs[] = {4, 5, 6, 7, 8, 9, 10, 11};
	int ri = first_reg;
	const char *f = rt_str(fmt_addr);
	size_t n = 0;
	while (*f && n + 1 < cap) {
		if (*f != '%') {
			out[n++] = *f++;
			continue;
		}
		char spec[16];
		int sl = 0;
		spec[sl++] = *f++;
		while (*f && strchr("-+ #0123456789.lh", *f) && sl < 14) {
			if (*f != 'l' && *f != 'h') {
				spec[sl++] = *f;
			}
			f++;
		}
		char conv = *f ? *f++ : 0;
		spec[sl++] = conv;
		spec[sl] = 0;
		uint32_t arg = (conv != '%' && ri < 8) ? c->r[regs[ri++]] : 0;
		char tmp[512];
		switch (conv) {
		case 's': snprintf(tmp, sizeof tmp, spec, rt_valid(arg, 1) ? rt_str(arg) : "(bad)"); break;
		case '%': snprintf(tmp, sizeof tmp, "%%"); break;
		case 'f': case 'g': case 'e': snprintf(tmp, sizeof tmp, "<float>"); break;
		default: snprintf(tmp, sizeof tmp, spec, arg); break;
		}
		for (char *p = tmp; *p && n + 1 < cap; p++) {
			out[n++] = *p;
		}
	}
	out[n] = 0;
}

void hle_sceKernelPrintf(CPU *c) {
	char buf[1024];
	guest_format(c, A0, 1, buf, sizeof buf);
	rt_log("printf: %s", buf);
	RET(0);
}

void hle_sceKernelCpuSuspendIntr(CPU *c) { RET(1); }
void hle_sceKernelCpuResumeIntr(CPU *c) { RET(0); }

void hle_sceKernelExitGame(CPU *c) {
	rt_log("sceKernelExitGame");
	exit(0);
}
void hle_sceKernelRegisterExitCallback(CPU *c) { RET(0); }

static uint32_t main_module_uid;
static uint32_t fake_module(void) { return obj_new(OBJ_MODULE, NULL); }

void hle_sceKernelLoadModule(CPU *c) {
	uint32_t uid = fake_module();
	rt_log("sceKernelLoadModule(%s) -> %08x (HLE, not loaded)", rt_str(A0), uid);
	RET(uid);
}
void hle_sceKernelStartModule(CPU *c) {
	if (A3) {
		M32(A3) = 0;
	}
	RET(A0);
}
void hle_sceKernelStopModule(CPU *c) {
	if (A3) {
		M32(A3) = 0;
	}
	RET(0);
}
void hle_sceKernelUnloadModule(CPU *c) { RET(0); }
void hle_sceKernelGetModuleId(CPU *c) {
	if (!main_module_uid) {
		main_module_uid = fake_module();
	}
	RET(main_module_uid);
}
void hle_sceKernelGetModuleIdByAddress(CPU *c) { hle_sceKernelGetModuleId(c); }
void hle_sceKernelStopUnloadSelfModuleWithStatus(CPU *c) {
	rt_log("sceKernelStopUnloadSelfModuleWithStatus(%x): game is exiting (ra=%08x)", A0, c->r[31]);
	exit((int)A0);
}

void hle_sceKernelStdin(CPU *c) { RET(0); }
void hle_sceKernelStdout(CPU *c) { RET(1); }
void hle_sceKernelStderr(CPU *c) { RET(2); }

void hle_sceKernelLibcClock(CPU *c) { RET((uint32_t)rt_now_us()); }
void hle_sceKernelLibcTime(CPU *c) {
	uint32_t t = (uint32_t)time(NULL);
	if (A0) {
		M32(A0) = t;
	}
	RET(t);
}
void hle_sceKernelLibcGettimeofday(CPU *c) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (A0) {
		M32(A0) = (uint32_t)tv.tv_sec;
		M32(A0 + 4) = (uint32_t)tv.tv_usec;
	}
	RET(0);
}
void hle_sceKernelDcacheWritebackAll(CPU *c) { RET(0); }
void hle_sceKernelDcacheWritebackInvalidateAll(CPU *c) { RET(0); }
void hle_sceKernelDcacheWritebackRange(CPU *c) { RET(0); }

void hle_sceKernelPowerTick(CPU *c) { RET(0); }
void hle_sceKernelPowerLock(CPU *c) { RET(0); }
void hle_sceKernelPowerUnlock(CPU *c) { RET(0); }
void hle_sceKernelVolatileMemLock(CPU *c) {
	if (A1) {
		M32(A1) = 0x08400000u;
	}
	if (A2) {
		M32(A2) = 0x00400000u;
	}
	RET(0);
}
void hle_sceKernelVolatileMemTryLock(CPU *c) { hle_sceKernelVolatileMemLock(c); }
void hle_sceKernelVolatileMemUnlock(CPU *c) { RET(0); }

void hle_scePowerRegisterCallback(CPU *c) { RET(0); }
void hle_scePowerUnregisterCallback(CPU *c) { RET(0); }
void hle_scePowerIsBatteryExist(CPU *c) { RET(1); }
void hle_scePowerIsPowerOnline(CPU *c) { RET(1); }
void hle_scePowerSetClockFrequency350(CPU *c) { RET(0); }

void hle_sceImposeSetLanguageMode(CPU *c) { RET(0); }

void hle_sceRtcGetCurrentTick(CPU *c) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	// PSP ticks: microseconds since 0001-01-01.
	uint64_t ticks = ((uint64_t)tv.tv_sec + 62135596800ull) * 1000000ull + (uint64_t)tv.tv_usec;
	M32(A0) = (uint32_t)ticks;
	M32(A0 + 4) = (uint32_t)(ticks >> 32);
	RET(0);
}

void hle_sceRtcGetCurrentClockLocalTime(CPU *c) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	struct tm tm;
	time_t s = tv.tv_sec;
	localtime_r(&s, &tm);
	uint32_t p = A0;
	M16(p + 0) = (uint16_t)(tm.tm_year + 1900);
	M16(p + 2) = (uint16_t)(tm.tm_mon + 1);
	M16(p + 4) = (uint16_t)tm.tm_mday;
	M16(p + 6) = (uint16_t)tm.tm_hour;
	M16(p + 8) = (uint16_t)tm.tm_min;
	M16(p + 10) = (uint16_t)tm.tm_sec;
	M32(p + 12) = (uint32_t)tv.tv_usec;
	RET(0);
}

// sceWlanGetEtherAddr: net.c (the ad hoc MAC)
void hle_sceWlanGetSwitchState(CPU *c) { RET(1); }
