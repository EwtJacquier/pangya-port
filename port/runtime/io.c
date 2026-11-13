// File I/O: the game disc is read straight from the user's ISO (ISO9660), the memory stick
// (ms0:) is a host directory. Implements IoFileMgrForUser and sceUmdUser.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hle.h"
#include "platform.h"
#include "rt.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// The ISO is a File/Blob chosen in the page (Module.isoBlob), read in slices.
EM_ASYNC_JS(int, js_iso_read, (double off, void *dst, int n), {
	const blob = Module.isoBlob;
	if (!blob) return -1;
	const ab = await blob.slice(off, off + n).arrayBuffer();
	HEAPU8.set(new Uint8Array(ab), dst);
	return ab.byteLength;
});
#endif

#define SECTOR 2048u
#define ERR_NOENT 0x80010002u
#define ERR_BADF 0x80010009u
#define ERR_INVAL 0x80010016u
#define ERR_MFILE 0x80010018u
#define ERR_NOTDIR 0x80010014u
#define ERR_NOCWD 0x8002032Cu

static int iso_fd = -1;
static uint32_t iso_sectors;
static char ms0_root[512];

typedef struct {
	uint32_t lba, size;
	bool dir;
} IsoEntry;

// Raw reads go through a small cache of 64 KB blocks: the game issues many tiny reads (a few
// bytes of a pak header), and on the web every backend read is an async round trip.
#define CHUNK (64u * 1024u)
#define NCHUNKS 128
static struct {
	uint64_t base;  // chunk index + 1 (0 = empty)
	uint64_t last_use;
	uint8_t data[CHUNK];
} *cache;
static uint64_t cache_clock;

#ifdef __EMSCRIPTEN__
typedef struct {
	double off;
	void *dst;
	int n, r;
} IsoRequest;
static void iso_request_run(void *p) {
	IsoRequest *q = p;
	q->r = js_iso_read(q->off, q->dst, q->n);
}
#endif

static void backend_read(uint64_t off, void *buf, size_t n) {
#ifdef __EMSCRIPTEN__
	// The read awaits a fetch/Blob promise: done on the scheduler's stack (see rt_call_on_main).
	IsoRequest q = {(double)off, buf, (int)n, 0};
	rt_call_on_main(iso_request_run, &q);
	int r = q.r;
	if (r < (int)n) memset((uint8_t *)buf + (r > 0 ? r : 0), 0, n - (size_t)(r > 0 ? r : 0));
#else
	ssize_t r = pread(iso_fd, buf, n, (off_t)off);
	if (r < (ssize_t)n) memset((uint8_t *)buf + (r > 0 ? r : 0), 0, n - (size_t)(r > 0 ? r : 0));
#endif
}

static void iso_read(uint64_t off, void *buf, size_t n) {
	if (n >= 4 * CHUNK) {  // large reads bypass the cache
		backend_read(off, buf, n);
		return;
	}
	if (!cache) cache = calloc(NCHUNKS, sizeof *cache);
	uint8_t *out = buf;
	while (n) {
		uint64_t idx = off / CHUNK;
		uint32_t in = (uint32_t)(off % CHUNK);
		size_t take = CHUNK - in < n ? CHUNK - in : n;
		int slot = -1, lru = 0;
		for (int i = 0; i < NCHUNKS; i++) {
			if (cache[i].base == idx + 1) {
				slot = i;
				break;
			}
			if (cache[i].last_use < cache[lru].last_use) lru = i;
		}
		if (slot < 0) {
			slot = lru;
			backend_read(idx * CHUNK, cache[slot].data, CHUNK);
			cache[slot].base = idx + 1;
		}
		cache[slot].last_use = ++cache_clock;
		memcpy(out, cache[slot].data + in, take);
		out += take;
		off += take;
		n -= take;
	}
}

static IsoEntry iso_root;

static bool iso_find_in_dir(IsoEntry dir, const char *name, size_t len, IsoEntry *out) {
	uint8_t *buf = malloc(dir.size);
	iso_read((uint64_t)dir.lba * SECTOR, buf, dir.size);
	bool found = false;
	for (uint32_t pos = 0; pos < dir.size;) {
		uint8_t rl = buf[pos];
		if (rl == 0) {
			pos = (pos / SECTOR + 1) * SECTOR;
			continue;
		}
		uint8_t nl = buf[pos + 32];
		const char *n = (const char *)buf + pos + 33;
		size_t effective = nl;
		for (size_t i = 0; i < nl; i++) {
			if (n[i] == ';') {
				effective = i;
				break;
			}
		}
		if (effective == len && strncasecmp(n, name, len) == 0) {
			memcpy(&out->lba, buf + pos + 2, 4);
			memcpy(&out->size, buf + pos + 10, 4);
			out->dir = buf[pos + 25] & 2;
			found = true;
			break;
		}
		pos += rl;
	}
	free(buf);
	return found;
}

static bool iso_lookup(const char *path, IsoEntry *out) {
	IsoEntry e = iso_root;
	while (*path == '/') {
		path++;
	}
	while (*path) {
		const char *slash = strchr(path, '/');
		size_t len = slash ? (size_t)(slash - path) : strlen(path);
		if (len && !(len == 1 && path[0] == '.')) {
			if (!e.dir || !iso_find_in_dir(e, path, len, &e)) {
				return false;
			}
		}
		path += len;
		while (*path == '/') {
			path++;
		}
	}
	*out = e;
	return true;
}

void io_init(const char *iso_path, const char *ms0_dir) {
#ifndef __EMSCRIPTEN__
	iso_fd = open(iso_path, O_RDONLY);
	if (iso_fd < 0) {
		rt_fatal("cannot open ISO %s: %s", iso_path, strerror(errno));
	}
#endif
	uint8_t pvd[SECTOR];
	iso_read(16 * SECTOR, pvd, SECTOR);
	if (memcmp(pvd + 1, "CD001", 5) != 0) {
		rt_fatal("%s is not an ISO9660 image", iso_path);
	}
	memcpy(&iso_sectors, pvd + 80, 4);
	memcpy(&iso_root.lba, pvd + 156 + 2, 4);
	memcpy(&iso_root.size, pvd + 156 + 10, 4);
	iso_root.dir = true;
	snprintf(ms0_root, sizeof ms0_root, "%s", ms0_dir);
	mkdir(ms0_root, 0755);
	char p[600];
	snprintf(p, sizeof p, "%s/PSP", ms0_root);
	mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/PSP/SAVEDATA", ms0_root);
	mkdir(p, 0755);
	rt_log("ISO %s: %u sectors; ms0 -> %s", iso_path, iso_sectors, ms0_root);
}

// ---- file descriptor table -------------------------------------------------------------
enum { F_NONE, F_ISO, F_BLOCKDEV, F_HOST, F_STDIO, F_DIR_ISO, F_DIR_HOST };
typedef struct {
	int type;
	uint32_t lba, size;  // F_ISO: byte-addressed region of the disc; F_BLOCKDEV: whole disc in sectors
	uint64_t pos;
	int host_fd;
	DIR *host_dir;
	char host_path[600];
	uint8_t *dir_buf;  // F_DIR_ISO: raw directory records
	uint32_t dir_pos;
	int64_t async_result;
	bool async_pending;
	char path[256];
} File;

#define MAX_FILES 64
static File files[MAX_FILES];

static int fd_alloc(void) {
	for (int i = 3; i < MAX_FILES; i++) {
		if (files[i].type == F_NONE) {
			memset(&files[i], 0, sizeof files[i]);
			files[i].host_fd = -1;
			return i;
		}
	}
	return -1;
}

static File *fd_get(uint32_t fd) {
	if (fd < 3 || fd >= MAX_FILES || files[fd].type == F_NONE) {
		return NULL;
	}
	return &files[fd];
}

// Split "dev:/path" into device and path; relative paths use the PSP default cwd.
static void split_path(const char *in, char *dev, size_t devcap, const char **rest) {
	const char *colon = strchr(in, ':');
	if (!colon) {
		snprintf(dev, devcap, "disc0");
		*rest = in;  // relative to disc root's PSP_GAME/USRDIR (resolved by caller)
		return;
	}
	size_t n = (size_t)(colon - in);
	if (n >= devcap) {
		n = devcap - 1;
	}
	memcpy(dev, in, n);
	dev[n] = 0;
	for (char *p = dev; *p; p++) {
		*p = (char)tolower(*p);
	}
	*rest = colon + 1;
}

static bool is_disc(const char *dev) {
	return !strcmp(dev, "disc0") || !strcmp(dev, "umd0") || !strcmp(dev, "umd1") || !strcmp(dev, "umd") ||
	       !strcmp(dev, "isofs0");
}

static void host_path(const char *rest, char *out, size_t cap) {
	while (*rest == '/') {
		rest++;
	}
	snprintf(out, cap, "%s/%s", ms0_root, rest);
}

// File I/O blocks the calling thread for a moment, as on the PSP, so lower-priority threads run
// meanwhile. The game depends on it: e.g. its BGM thread opens and reads the track and then
// raises a "ready" event flag, while the starter thread clears that flag right after
// sceKernelStartThread returns. With instant I/O the flag was set first, cleared, and the game
// waited ~8 s for a timeout at every music change (menus, after the opening movie).
static void io_latency(uint32_t us) { rt_wait_until(rt_now_us() + us); }

void hle_sceIoOpen(CPU *c) {
	io_latency(200);
	const char *path = rt_str(A0);
	uint32_t flags = A1;
	char dev[16];
	const char *rest;
	if (!strchr(path, ':')) {
		// Relative path and threads have no current directory: the game relies on this error
		// to fall back to its pak archive (FUN_08b005e0 loops forever on any other error).
		TRACE("sceIoOpen(%s) -> NOCWD", path);
		RET(ERR_NOCWD);
		return;
	}
	split_path(path, dev, sizeof dev, &rest);
	int fd = fd_alloc();
	if (fd < 0) {
		RET(ERR_MFILE);
		return;
	}
	File *f = &files[fd];
	snprintf(f->path, sizeof f->path, "%s", path);

	if (is_disc(dev)) {
		uint32_t lbn, size;
		if (*rest == 0 || !strcmp(rest, "/")) {
			f->type = F_BLOCKDEV;
			f->lba = 0;
			f->size = iso_sectors;
		}
		else if (sscanf(rest, "/sce_lbn0x%x_size0x%x", &lbn, &size) == 2 ||
		         sscanf(rest, "/sce_lbn%u_size%u", &lbn, &size) == 2) {
			f->type = F_ISO;
			f->lba = lbn;
			f->size = size;
		}
		else {
			IsoEntry e;
			char full[300];
			if (!strchr(path, ':')) {
				snprintf(full, sizeof full, "/PSP_GAME/USRDIR/%s", rest);
				rest = full;
			}
			if (!iso_lookup(rest, &e) || e.dir) {
				TRACE("sceIoOpen(%s) -> not found", path);
				RET(ERR_NOENT);
				return;
			}
			f->type = F_ISO;
			f->lba = e.lba;
			f->size = e.size;
		}
	}
	else if (!strcmp(dev, "ms0") || !strcmp(dev, "fatms0")) {
		host_path(rest, f->host_path, sizeof f->host_path);
		int oflags = 0;
		switch (flags & 3) {
		case 1: oflags = O_RDONLY; break;
		case 2: oflags = O_WRONLY; break;
		default: oflags = O_RDWR; break;
		}
		if (flags & 0x0100) oflags |= O_APPEND;
		if (flags & 0x0200) oflags |= O_CREAT;
		if (flags & 0x0400) oflags |= O_TRUNC;
		if (flags & 0x0800) oflags |= O_EXCL;
		f->host_fd = open(f->host_path, oflags, 0644);
		if (f->host_fd < 0) {
			TRACE("sceIoOpen(%s) -> host %s: %s", path, f->host_path, strerror(errno));
			RET(ERR_NOENT);
			return;
		}
		f->type = F_HOST;
	}
	else {
		rt_log("sceIoOpen(%s): unsupported device", path);
		RET(ERR_NOENT);
		return;
	}
	TRACE("sceIoOpen(%s, %x) -> %d (lba %u size %u)", path, flags, fd, f->lba, f->size);
	RET(fd);
}

void hle_sceIoClose(CPU *c) {
	File *f = fd_get(A0);
	if (!f) {
		RET(ERR_BADF);
		return;
	}
	if (f->host_fd >= 0) {
		close(f->host_fd);
		plat_persist();
	}
	f->type = F_NONE;
	RET(0);
}

// Recent disc reads (guest destination -> ISO byte offset), so decoders can find where a buffer
// of streamed data came from and read the whole file themselves.
typedef struct {
	uint32_t dest, len;
	uint64_t iso_off;
} ReadRec;
static ReadRec read_log[128];
static int read_log_pos;

static void log_read(uint32_t dest, uint32_t len, uint64_t iso_off) {
	read_log[read_log_pos] = (ReadRec){dest, len, iso_off};
	read_log_pos = (read_log_pos + 1) % 128;
}

bool io_find_source(uint32_t addr, uint64_t *iso_off) {
	for (int i = 1; i <= 128; i++) {
		ReadRec *r = &read_log[(read_log_pos - i + 128) % 128];
		if (r->len && addr >= r->dest && addr < r->dest + r->len) {
			*iso_off = r->iso_off + (addr - r->dest);
			return true;
		}
	}
	return false;
}

void io_iso_read(uint64_t off, void *dst, size_t n) { iso_read(off, dst, n); }

// Byte offset and size of a disc file ("disc0:/PSP_GAME/...").
bool io_disc_lookup(const char *path, uint64_t *off, uint32_t *size) {
	const char *colon = strchr(path, ':');
	IsoEntry e;
	if (!iso_lookup(colon ? colon + 1 : path, &e) || e.dir) return false;
	*off = (uint64_t)e.lba * SECTOR;
	*size = e.size;
	return true;
}

static int64_t do_read(File *f, uint32_t buf, uint32_t size) {
	if (f->type == F_BLOCKDEV) {  // size and position in sectors
		if (f->pos >= f->size) {
			return 0;
		}
		if (f->pos + size > f->size) {
			size = (uint32_t)(f->size - f->pos);
		}
		iso_read(f->pos * SECTOR, MP(buf), (size_t)size * SECTOR);
		log_read(buf, size * SECTOR, f->pos * SECTOR);
		f->pos += size;
		return size;
	}
	if (f->type == F_ISO) {
		if (f->pos >= f->size) {
			return 0;
		}
		if (f->pos + size > f->size) {
			size = (uint32_t)(f->size - f->pos);
		}
		iso_read((uint64_t)f->lba * SECTOR + f->pos, MP(buf), size);
		log_read(buf, size, (uint64_t)f->lba * SECTOR + f->pos);
		f->pos += size;
		return size;
	}
	if (f->type == F_HOST) {
		ssize_t n = read(f->host_fd, MP(buf), size);
		return n < 0 ? (int64_t)ERR_INVAL : n;
	}
	return (int64_t)ERR_BADF;
}

void hle_sceIoRead(CPU *c) {
	File *f = fd_get(A0);
	if (!f) {
		RET(ERR_BADF);
		return;
	}
	io_latency(50 + A2 / 200);  // ~200 MB/s: keeps loads fast
	int64_t r = do_read(f, A1, A2);
	TRACE("sceIoRead(%d %s, %08x, %x) -> %llx", A0, f->path, A1, A2, (long long)r);
	RET(r);
}

void hle_sceIoWrite(CPU *c) {
	if (A0 == 1 || A0 == 2) {
		fprintf(stderr, "%.*s", (int)A2, (const char *)MP(A1));
		RET(A2);
		return;
	}
	File *f = fd_get(A0);
	if (!f || f->type != F_HOST) {
		RET(ERR_BADF);
		return;
	}
	ssize_t n = write(f->host_fd, MP(A1), A2);
	RET(n < 0 ? ERR_INVAL : (uint32_t)n);
}

static int64_t do_seek(File *f, int64_t off, uint32_t whence) {
	int64_t base = whence == 1 ? (int64_t)f->pos : whence == 2 ? (int64_t)f->size : 0;
	if (f->type == F_HOST) {
		off_t r = lseek(f->host_fd, (off_t)off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
		return r < 0 ? (int64_t)ERR_INVAL : r;
	}
	int64_t np = base + off;
	if (np < 0) {
		return (int64_t)(int32_t)ERR_INVAL;
	}
	f->pos = (uint64_t)np;
	return np;
}

void hle_sceIoLseek(CPU *c) {
	File *f = fd_get(A0);
	if (!f) {
		RET64((int64_t)(int32_t)ERR_BADF);
		return;
	}
	int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);
	int64_t r = do_seek(f, off, A4);
	TRACE("sceIoLseek(%d, %lld, %d) -> %lld", A0, (long long)off, A4, (long long)r);
	RET64(r);
}

// Copy protection: FUN_0887f670 times two raw sector reads on umd1: (sectors 0 and 0x64000) and
// treats reads faster than 15 ms / 20 ms as a copied disc (a real UMD drive has to seek).
// Reads issued from that function get real-UMD latency; nothing else is slowed down.
static bool in_umd_speed_check(CPU *c) { return c->r[31] >= 0x0887f670u && c->r[31] < 0x0887ffd4u; }

void hle_sceIoIoctl(CPU *c) {
	File *f = fd_get(A0);
	uint32_t cmd = A1, in = A2, inlen = A3, out = A4, outlen = A5;
	if (!f) {
		RET(ERR_BADF);
		return;
	}
	TRACE("sceIoIoctl(%d %s, %08x, in=%08x/%x, out=%08x/%x)", A0, f->path, cmd, in, inlen, out, outlen);
	switch (cmd) {
	case 0x01F100A6: {  // seek (sectors on the block device)
		int64_t off = (int64_t)(((uint64_t)M32(in + 4) << 32) | M32(in));
		uint32_t whence = M32(in + 12);
		int64_t r = do_seek(f, off, whence);
		TRACE("  sector seek %lld whence %d -> %lld", (long long)off, whence, (long long)r);
		RET(r < 0 ? (uint32_t)r : 0);
		return;
	}
	case 0x01F30003: {  // read sectors
		uint32_t n = M32(in);
		if (f->type == F_BLOCKDEV && in_umd_speed_check(c)) {
			rt_wait_until(rt_now_us() + 25000);
		}
		int64_t r = do_read(f, out, n);
		TRACE("  sector read %u -> %lld: %02x %02x %02x %02x %02x %02x %02x %02x", n, (long long)r, M8(out), M8(out + 1),
		      M8(out + 2), M8(out + 3), M8(out + 4), M8(out + 5), M8(out + 6), M8(out + 7));
		RET(r);
		return;
	}
	case 0x01D20001:  // current sector
		M32(out) = (uint32_t)f->pos;
		RET(0);
		return;
	case 0x01020006:  // start sector of a file
		M32(out) = f->lba;
		RET(0);
		return;
	case 0x01020007:  // file size in bytes
		M32(out) = f->size;
		M32(out + 4) = 0;
		RET(0);
		return;
	case 0x01030008:  // read bytes
		RET(do_read(f, out, M32(in)));
		return;
	case 0x01F010DB:  // UMD cache control
	case 0x01F100A8:
		RET(0);
		return;
	default:
		rt_log("sceIoIoctl(%d %s): unknown cmd %08x", A0, f->path, cmd);
		RET(0);
		return;
	}
}

void hle_sceIoReadAsync(CPU *c) {
	File *f = fd_get(A0);
	if (!f) {
		RET(ERR_BADF);
		return;
	}
	f->async_result = do_read(f, A1, A2);
	f->async_pending = true;
	TRACE("sceIoReadAsync(%d %s, %08x, %x) -> %llx", A0, f->path, A1, A2, (long long)f->async_result);
	RET(0);
}

static void async_wait(CPU *c) {
	File *f = fd_get(A0);
	if (!f) {
		RET(ERR_BADF);
		return;
	}
	if (A1) {
		M32(A1) = (uint32_t)f->async_result;
		M32(A1 + 4) = (uint32_t)((uint64_t)f->async_result >> 32);
	}
	f->async_pending = false;
	RET(0);
}
void hle_sceIoWaitAsync(CPU *c) { async_wait(c); }
void hle_sceIoWaitAsyncCB(CPU *c) { async_wait(c); }
void hle_sceIoPollAsync(CPU *c) {
	File *f = fd_get(A0);
	if (f && !f->async_pending) {
		RET(1);  // no pending operation
		return;
	}
	async_wait(c);
}

// ---- stat / directories -----------------------------------------------------------------

static void write_stat(uint32_t p, bool dir, uint64_t size) {
	memset(MP(p), 0, 0x58);
	M32(p + 0) = dir ? 0x11FFu : 0x21FFu;  // FIO_S_IFDIR/IFREG | 0777
	M32(p + 4) = dir ? 0x10u : 0x20u;
	M32(p + 8) = (uint32_t)size;
	M32(p + 12) = (uint32_t)(size >> 32);
	for (int i = 0; i < 3; i++) {  // ctime/atime/mtime: 2008-01-01
		M16(p + 16 + i * 16) = 2008;
		M16(p + 18 + i * 16) = 1;
		M16(p + 20 + i * 16) = 1;
	}
}

void hle_sceIoGetstat(CPU *c) {
	const char *path = rt_str(A0);
	char dev[16];
	const char *rest;
	split_path(path, dev, sizeof dev, &rest);
	if (is_disc(dev)) {
		IsoEntry e;
		if (!iso_lookup(rest, &e)) {
			RET(ERR_NOENT);
			return;
		}
		write_stat(A1, e.dir, e.size);
		RET(0);
		return;
	}
	char hp[600];
	host_path(rest, hp, sizeof hp);
	struct stat st;
	if (stat(hp, &st) != 0) {
		TRACE("sceIoGetstat(%s) -> not found", path);
		RET(ERR_NOENT);
		return;
	}
	write_stat(A1, S_ISDIR(st.st_mode), (uint64_t)st.st_size);
	RET(0);
}

void hle_sceIoDopen(CPU *c) {
	const char *path = rt_str(A0);
	char dev[16];
	const char *rest;
	split_path(path, dev, sizeof dev, &rest);
	int fd = fd_alloc();
	if (fd < 0) {
		RET(ERR_MFILE);
		return;
	}
	File *f = &files[fd];
	snprintf(f->path, sizeof f->path, "%s", path);
	if (is_disc(dev)) {
		IsoEntry e;
		if (!iso_lookup(rest, &e) || !e.dir) {
			RET(ERR_NOENT);
			return;
		}
		f->type = F_DIR_ISO;
		f->dir_buf = malloc(e.size);
		f->size = e.size;
		iso_read((uint64_t)e.lba * SECTOR, f->dir_buf, e.size);
	}
	else {
		host_path(rest, f->host_path, sizeof f->host_path);
		f->host_dir = opendir(f->host_path);
		if (!f->host_dir) {
			TRACE("sceIoDopen(%s) -> not found", path);
			RET(ERR_NOENT);
			return;
		}
		f->type = F_DIR_HOST;
	}
	TRACE("sceIoDopen(%s) -> %d", path, fd);
	RET(fd);
}

void hle_sceIoDread(CPU *c) {
	File *f = fd_get(A0);
	uint32_t ent = A1;
	if (!f) {
		RET(ERR_BADF);
		return;
	}
	memset(MP(ent), 0, 0x58 + 256);
	if (f->type == F_DIR_HOST) {
		struct dirent *de = readdir(f->host_dir);
		if (!de) {
			RET(0);
			return;
		}
		char p[1200];
		snprintf(p, sizeof p, "%s/%s", f->host_path, de->d_name);
		struct stat st;
		stat(p, &st);
		write_stat(ent, S_ISDIR(st.st_mode), (uint64_t)st.st_size);
		rt_write_cstr(ent + 0x58, 256, de->d_name);
		RET(1);
		return;
	}
	if (f->type == F_DIR_ISO) {
		while (f->dir_pos < f->size) {
			uint8_t rl = f->dir_buf[f->dir_pos];
			if (!rl) {
				f->dir_pos = (f->dir_pos / SECTOR + 1) * SECTOR;
				continue;
			}
			uint8_t *r = f->dir_buf + f->dir_pos;
			f->dir_pos += rl;
			uint8_t nl = r[32];
			char name[256];
			if (nl == 1 && (r[33] == 0 || r[33] == 1)) {
				snprintf(name, sizeof name, "%s", r[33] ? ".." : ".");
			}
			else {
				snprintf(name, sizeof name, "%.*s", nl, r + 33);
				char *semi = strchr(name, ';');
				if (semi) {
					*semi = 0;
				}
			}
			uint32_t size;
			memcpy(&size, r + 10, 4);
			write_stat(ent, r[25] & 2, size);
			rt_write_cstr(ent + 0x58, 256, name);
			RET(1);
			return;
		}
		RET(0);
		return;
	}
	RET(ERR_NOTDIR);
}

void hle_sceIoDclose(CPU *c) {
	File *f = fd_get(A0);
	if (!f) {
		RET(ERR_BADF);
		return;
	}
	if (f->host_dir) {
		closedir(f->host_dir);
	}
	free(f->dir_buf);
	f->type = F_NONE;
	RET(0);
}

void hle_sceIoRename(CPU *c) {
	char dev[16], a[600], b[600];
	const char *ra, *rb;
	split_path(rt_str(A0), dev, sizeof dev, &ra);
	split_path(rt_str(A1), dev, sizeof dev, &rb);
	host_path(ra, a, sizeof a);
	host_path(rb, b, sizeof b);
	RET(rename(a, b) == 0 ? 0 : ERR_NOENT);
}

// ---- UMD --------------------------------------------------------------------------------
void hle_sceUmdActivate(CPU *c) { RET(0); }
void hle_sceUmdGetDriveStat(CPU *c) { RET(0x32); }  // present | ready | readable
void hle_sceUmdWaitDriveStat(CPU *c) { RET(0); }
void hle_sceUmdWaitDriveStatCB(CPU *c) { RET(0); }
void hle_sceUmdCancelWaitDriveStat(CPU *c) { RET(0); }
