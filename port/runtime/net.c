// sceNet / ad hoc on top of the host transport (platform.h: LiveKit in the browser, UDP on
// 127.0.0.1 natively). Every copy of the game is a PSP with a virtual MAC (locally administered,
// random per run, or PANGYA_MAC); the ad hoc "air" is the set of copies that share a room.
//
// Nodes tell each other their state with HELLO messages (group joined, nickname): on joining the
// room (asking everyone to answer), and whenever their group changes. A scan sends a PROBE that
// everyone answers. From that each copy knows the groups around it (scan results) and the
// members of its own group (peer list). PDP datagrams travel as DATA messages tagged with the
// sender's group; a copy keeps only those of its own group, for a port it has a socket on.
//
// The game (FUN_08b03010 & co.) registers one handler and waits for its flags after Connect /
// Scan / Disconnect, polling once a second; the handler is called before those calls return, so
// the waits end at once. Scan and Connect first wait (a few seconds at most) for the transport.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "platform.h"
#include "rt.h"

#define PEER_MAX 16
#define SOCK_MAX 8
#define PDP_MAX 2048
#define QUEUE_MAX 128
#define HANDLER_MAX 4

#define ERR_WOULD_BLOCK 0x80410709u
#define ERR_TIMEOUT 0x80410715u
#define ERR_NOT_ENOUGH_SPACE 0x80400706u
#define ERR_NO_ENTRY 0x80410716u
#define ERR_INVALID_ARG 0x80410702u
#define ERR_INVALID_SOCKET 0x80410703u

#define DONE(v) \
	do { \
		RET(v); \
		return; \
	} while (0)

#define SCAN_ENTRY 28   // next, channel, group[8], bssid[6] + 2, mode
#define PEER_ENTRY 152  // next, nickname[128], mac[6] + 2, flags, last_recv (u64)

enum { MSG_HELLO = 'H', MSG_PROBE = 'P', MSG_DATA = 'D', MSG_MOD = 'X' };
#define HELLO_SIZE 138  // type, flags (1 answer me, 2 in a group), group[8], nickname[128]
#define DATA_HEAD 14    // type, 0, source port, destination port (u16 LE), group[8]
#define MOD_HEAD 10     // type, kind, group[8]: the mods' own messages (net_mod_send)
#define MOD_MAX 512
#define MOD_QUEUE 256  // a copy held back a while gets several players' shots, results and turns at once

typedef struct {
	bool used, in_group;
	uint8_t mac[6];
	char group[8];
	char nick[128];
	uint64_t taken_at;  // when its last datagram was taken: vblank + 1, or a receive round (0: none yet)
} Peer;

typedef struct {
	uint8_t src[6];
	uint16_t sport, dport;
	int len;
	uint8_t data[PDP_MAX];
} Packet;

static Peer peers[PEER_MAX];
static bool in_group;
static char group[8];
static int last_state = NET_OFF;
static struct {
	bool used;
	uint32_t func, arg;
} handlers[HANDLER_MAX];
static struct {
	bool used;
	uint16_t port;
} socks[SOCK_MAX];
static Packet queue[QUEUE_MAX];
static int q_first, q_len;
static uint64_t round_no;  // the game's receive rounds (net_recv_round), from 1 << 63 up
static bool in_round;
static struct {
	char group[8];
	uint8_t bssid[6];
} scan[PEER_MAX];
static int n_scan;

static struct {
	uint8_t from[6];
	int kind, len;
	uint8_t data[MOD_MAX];
} modq[MOD_QUEUE];
static int mq_len;

static const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// ---- identity ----------------------------------------------------------------------------------
static const uint8_t *my_mac(void) {
	static uint8_t mac[6];
	static bool ready;
	if (ready) return mac;
	ready = true;
	unsigned v[6];
	const char *e = getenv("PANGYA_MAC");
	if (e && sscanf(e, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
		for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
	} else {
		FILE *f = fopen("/dev/urandom", "rb");
		if (!f || fread(mac, 1, 6, f) != 6) {
			uint64_t t = rt_now_us() ^ (uint64_t)(uintptr_t)&mac;
			for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(t >> (8 * i));
		}
		if (f) fclose(f);
	}
	mac[0] = (uint8_t)((mac[0] & 0xfc) | 0x02);  // locally administered, unicast
	return mac;
}

static const char *my_nick(void) {
	const char *n = getenv("PANGYA_NICK");
	return n && *n ? n : "Player";
}

static const char *mac_str(const uint8_t *m) {
	static char s[4][18];
	static int k;
	k = (k + 1) & 3;
	snprintf(s[k], 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	return s[k];
}

// ---- the air -----------------------------------------------------------------------------------
static Peer *peer_find(const uint8_t *mac) {
	for (int i = 0; i < PEER_MAX; i++)
		if (peers[i].used && !memcmp(peers[i].mac, mac, 6)) return &peers[i];
	return NULL;
}

static bool same_group(const Peer *p) { return in_group && p->used && p->in_group && !memcmp(p->group, group, 8); }

static void send_hello(const uint8_t *to, bool answer_me) {
	uint8_t m[HELLO_SIZE] = {MSG_HELLO, (uint8_t)((answer_me ? 1 : 0) | (in_group ? 2 : 0))};
	memcpy(m + 2, group, 8);
	strncpy((char *)m + 10, my_nick(), 127);
	plat_net_send(to, m, sizeof m);
}

static void on_hello(const uint8_t *from, const uint8_t *m) {
	Peer *p = peer_find(from);
	if (!p)
		for (int i = 0; i < PEER_MAX && !p; i++)
			if (!peers[i].used) p = &peers[i], memset(p, 0, sizeof *p), p->used = true, memcpy(p->mac, from, 6);
	if (!p) return;
	bool was_in = p->in_group;
	char was[8];
	memcpy(was, p->group, 8);
	p->in_group = m[1] & 2;
	memcpy(p->group, m + 2, 8);
	memcpy(p->nick, m + 10, 127);
	p->nick[127] = 0;
	if (p->in_group != was_in || (p->in_group && memcmp(was, p->group, 8)))
		rt_log("net: %s (%s) %s%.8s", mac_str(from), p->nick, p->in_group ? "is in group " : "left its group",
		       p->in_group ? p->group : "");
	if (m[1] & 1) send_hello(from, false);
}

static void on_data(const uint8_t *from, const uint8_t *m, int n) {
	if (!in_group || memcmp(m + 6, group, 8)) return;
	if (n - DATA_HEAD > PDP_MAX) n = DATA_HEAD + PDP_MAX;
	uint16_t sport = (uint16_t)(m[2] | m[3] << 8), dport = (uint16_t)(m[4] | m[5] << 8);
	bool bound = false;
	for (int i = 0; i < SOCK_MAX; i++) bound |= socks[i].used && socks[i].port == dport;
	if (!bound) return;
	if (q_len == QUEUE_MAX) q_first = (q_first + 1) % QUEUE_MAX, q_len--;  // full: drop the oldest
	Packet *q = &queue[(q_first + q_len++) % QUEUE_MAX];
	memcpy(q->src, from, 6);
	q->sport = sport;
	q->dport = dport;
	q->len = n - DATA_HEAD;
	memcpy(q->data, m + DATA_HEAD, (size_t)q->len);
}

static void on_mod(const uint8_t *from, const uint8_t *m, int n) {
	if (!in_group || memcmp(m + 2, group, 8) || n - MOD_HEAD > MOD_MAX) return;
	if (mq_len == MOD_QUEUE) memmove(&modq[0], &modq[1], sizeof modq[0] * (MOD_QUEUE - 1)), mq_len--;
	memcpy(modq[mq_len].from, from, 6);
	modq[mq_len].kind = m[1];
	modq[mq_len].len = n - MOD_HEAD;
	memcpy(modq[mq_len].data, m + MOD_HEAD, (size_t)(n - MOD_HEAD));
	mq_len++;
}

// Takes in what the transport received.
static void net_pump(void) {
	int st = plat_net_state();
	if (st != last_state) {
		if (st == NET_UP) rt_log("net: on the air as %s (%s)", mac_str(my_mac()), my_nick());
		if (st == NET_FAILED) rt_log("net: no transport; ad hoc runs alone");
		if (st == NET_UP) send_hello(NULL, true);  // and everyone answers with theirs
		last_state = st;
	}
	static uint8_t m[DATA_HEAD + PDP_MAX];
	uint8_t from[6];
	int n;
	while ((n = plat_net_recv(from, m, sizeof m)) != 0) {
		if (n < 0) {
			Peer *p = peer_find(from);
			if (p) rt_log("net: %s (%s) went away", mac_str(from), p->nick), p->used = false;
		} else if (m[0] == MSG_HELLO && n >= HELLO_SIZE) {
			on_hello(from, m);
		} else if (m[0] == MSG_PROBE) {
			send_hello(from, false);
		} else if (m[0] == MSG_DATA && n >= DATA_HEAD) {
			on_data(from, m, n);
		} else if (m[0] == MSG_MOD && n >= MOD_HEAD) {
			on_mod(from, m, n);
		}
	}
}

static void net_start(void) {
	const char *room = getenv("PANGYA_ROOM");
	if (plat_net_state() != NET_UP && plat_net_state() != NET_CONNECTING)
		plat_net_start(my_mac(), room && *room ? room : "pangya");
}

// Lets the transport work (and takes in what arrives) for `us`.
static void net_wait(uint64_t us) {
	uint64_t end = rt_now_us() + us;
	for (uint64_t now; (now = rt_now_us()) < end;) {
		net_pump();
		rt_wait_until(end - now < 10000 ? end : now + 10000);
	}
	net_pump();
}

static void wait_transport(void) {
	uint64_t end = rt_now_us() + 8000000;
	while (plat_net_state() == NET_CONNECTING && rt_now_us() < end) rt_wait_until(rt_now_us() + 20000);
	net_pump();
}

static void notify(int event) {
	for (int i = 0; i < HANDLER_MAX; i++)
		if (handlers[i].used) rt_call_guest(handlers[i].func, (uint32_t)event, 0, handlers[i].arg);
}

// ---- sceNet ------------------------------------------------------------------------------------
void hle_sceNetInit(CPU *c) { RET(0); }
void hle_sceNetTerm(CPU *c) { RET(0); }
void hle_sceNetEtherNtostr(CPU *c) {
	rt_write_cstr(A1, 18, mac_str(MP(A0)));
	RET(0);
}
void hle_sceNetEtherStrton(CPU *c) {
	unsigned v[6] = {0};
	sscanf(rt_str(A0), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
	for (int i = 0; i < 6; i++) M8(A1 + (uint32_t)i) = (uint8_t)v[i];
	RET(0);
}
void hle_sceWlanGetEtherAddr(CPU *c) {
	memcpy(MP(A0), my_mac(), 6);
	RET(0);
}

// ---- sceNetAdhocctl ----------------------------------------------------------------------------
void hle_sceNetAdhocctlInit(CPU *c) {
	net_start();
	net_pump();
	RET(0);
}

void hle_sceNetAdhocctlTerm(CPU *c) {
	if (in_group) {
		in_group = false;
		memset(group, 0, sizeof group);
		send_hello(NULL, false);
	}
	plat_net_stop();
	memset(peers, 0, sizeof peers);
	q_len = 0;
	last_state = NET_OFF;
	RET(0);
}

void hle_sceNetAdhocctlAddHandler(CPU *c) {
	for (int i = 0; i < HANDLER_MAX; i++)
		if (handlers[i].used && handlers[i].func == A0) {
			handlers[i].arg = A1;
			RET(i);
			return;
		}
	for (int i = 0; i < HANDLER_MAX; i++)
		if (!handlers[i].used) {
			handlers[i].used = true;
			handlers[i].func = A0;
			handlers[i].arg = A1;
			RET(i);
			return;
		}
	RET(0x80410b0au);  // too many handlers
}

void hle_sceNetAdhocctlDelHandler(CPU *c) {
	if (A0 < HANDLER_MAX) handlers[A0].used = false;
	RET(0);
}

void hle_sceNetAdhocctlScan(CPU *c) {
	net_start();
	wait_transport();
	if (plat_net_state() == NET_UP) {
		uint8_t probe = MSG_PROBE;
		plat_net_send(NULL, &probe, 1);
		net_wait(500000);
	}
	// one entry per group around; its BSSID is the lowest MAC in it (the PSP that made it, as far
	// as anyone can tell)
	n_scan = 0;
	for (int i = 0; i < PEER_MAX; i++) {
		Peer *p = &peers[i];
		if (!p->used || !p->in_group) continue;
		int k = 0;
		while (k < n_scan && memcmp(scan[k].group, p->group, 8)) k++;
		if (k == n_scan) {
			n_scan++;
			memcpy(scan[k].group, p->group, 8);
			memcpy(scan[k].bssid, p->mac, 6);
		} else if (memcmp(p->mac, scan[k].bssid, 6) < 0) {
			memcpy(scan[k].bssid, p->mac, 6);
		}
	}
	rt_log("net: scan found %d group(s)", n_scan);
	for (int k = 0; k < n_scan; k++) rt_log("net:   %.8s", scan[k].group);
	notify(3);
	RET(0);
}

void hle_sceNetAdhocctlGetScanInfo(CPU *c) {
	if (!A0) DONE(ERR_INVALID_ARG);
	int n = n_scan;
	if (!A1) {
		M32(A0) = (uint32_t)(n * SCAN_ENTRY);
		DONE(0);
	}
	int cap = (int)M32(A0) / SCAN_ENTRY;
	if (n > cap) n = cap;
	for (int i = 0; i < n; i++) {
		uint32_t e = A1 + (uint32_t)(i * SCAN_ENTRY);
		memset(MP(e), 0, SCAN_ENTRY);
		M32(e) = i + 1 < n ? e + SCAN_ENTRY : 0;
		M32(e + 4) = 1;  // channel
		memcpy(MP(e + 8), scan[i].group, 8);
		memcpy(MP(e + 16), scan[i].bssid, 6);
		M32(e + 24) = 0;  // mode: not 1 (the game skips those)
	}
	M32(A0) = (uint32_t)(n * SCAN_ENTRY);
	RET(0);
}

void hle_sceNetAdhocctlConnect(CPU *c) {
	net_start();
	wait_transport();
	memcpy(group, MP(A0), 8);
	in_group = true;
	if (plat_net_state() == NET_UP) {
		send_hello(NULL, true);
		net_wait(300000);  // the group's members answer
	}
	int n = 0;
	for (int i = 0; i < PEER_MAX; i++) n += same_group(&peers[i]);
	rt_log("net: joined group %.8s with %d other(s)", group, n);
	notify(1);
	RET(0);
}

void hle_sceNetAdhocctlDisconnect(CPU *c) {
	if (in_group) {
		rt_log("net: left group %.8s", group);
		in_group = false;
		memset(group, 0, sizeof group);
		send_hello(NULL, false);
	}
	q_len = 0;
	mq_len = 0;
	notify(2);
	RET(0);
}

void hle_sceNetAdhocctlGetPeerList(CPU *c) {
	if (!A0) DONE(ERR_INVALID_ARG);
	net_pump();
	Peer *list[PEER_MAX];
	int n = 0;
	for (int i = 0; i < PEER_MAX; i++)
		if (same_group(&peers[i])) list[n++] = &peers[i];
	if (!A1) {
		M32(A0) = (uint32_t)(n * PEER_ENTRY);
		DONE(0);
	}
	int cap = (int)M32(A0) / PEER_ENTRY;
	if (n > cap) n = cap;
	uint64_t now = rt_now_us();
	for (int i = 0; i < n; i++) {
		uint32_t e = A1 + (uint32_t)(i * PEER_ENTRY);
		memset(MP(e), 0, PEER_ENTRY);
		M32(e) = i + 1 < n ? e + PEER_ENTRY : 0;
		memcpy(MP(e + 4), list[i]->nick, 128);
		memcpy(MP(e + 132), list[i]->mac, 6);
		M32(e + 144) = (uint32_t)now;
		M32(e + 148) = (uint32_t)(now >> 32);
	}
	M32(A0) = (uint32_t)(n * PEER_ENTRY);
	RET(0);
}

void hle_sceNetAdhocctlGetNameByAddr(CPU *c) {
	const uint8_t *mac = MP(A0);
	Peer *p = peer_find(mac);
	if (!memcmp(mac, my_mac(), 6)) rt_write_cstr(A1, 128, my_nick());
	else if (p) rt_write_cstr(A1, 128, p->nick);
	else DONE(ERR_NO_ENTRY);
	RET(0);
}

// SceNetAdhocctlParams: channel, group name[8], bssid[6], nickname[128].
void hle_sceNetAdhocctlGetParameter(CPU *c) {
	uint32_t p = A0;
	memset(MP(p), 0, 146);
	M32(p) = 1;
	if (in_group) {
		memcpy(MP(p + 4), group, 8);
		const uint8_t *bssid = my_mac();
		for (int i = 0; i < PEER_MAX; i++)
			if (same_group(&peers[i]) && memcmp(peers[i].mac, bssid, 6) < 0) bssid = peers[i].mac;
		memcpy(MP(p + 12), bssid, 6);
	}
	rt_write_cstr(p + 18, 128, my_nick());
	RET(0);
}

// ---- sceNetAdhoc (PDP) -------------------------------------------------------------------------
void hle_sceNetAdhocInit(CPU *c) { RET(0); }
void hle_sceNetAdhocTerm(CPU *c) { RET(0); }

void hle_sceNetAdhocPdpCreate(CPU *c) {
	uint16_t port = (uint16_t)A1;
	for (int i = 0; i < SOCK_MAX; i++)  // the game makes its socket again on each connect
		if (socks[i].used && socks[i].port == port) DONE(i + 1);
	for (int i = 0; i < SOCK_MAX; i++)
		if (!socks[i].used) {
			socks[i].used = true;
			socks[i].port = port;
			DONE(i + 1);
		}
	RET(0x80410708u);
}

static int sock_index(uint32_t id) { return id >= 1 && id <= SOCK_MAX && socks[id - 1].used ? (int)id - 1 : -1; }

// The game deletes its socket again when it leaves a match; failing that would skip its
// sceNetAdhocctlDisconnect (FUN_08b03418), so a socket that is already gone is fine.
void hle_sceNetAdhocPdpDelete(CPU *c) {
	int s = sock_index(A0);
	if (s >= 0) socks[s].used = false;
	RET(0);
}

// (id, destination MAC, port, data, length, timeout, nonblock)
void hle_sceNetAdhocPdpSend(CPU *c) {
	int s = sock_index(A0);
	int len = (int)A4;
	if (s < 0) DONE(ERR_INVALID_SOCKET);
	if (len < 0 || len > PDP_MAX) DONE(ERR_INVALID_ARG);
	net_pump();
	static uint8_t m[DATA_HEAD + PDP_MAX];
	uint16_t sport = socks[s].port, dport = (uint16_t)A2;
	m[0] = MSG_DATA;
	m[1] = 0;
	m[2] = (uint8_t)sport, m[3] = (uint8_t)(sport >> 8);
	m[4] = (uint8_t)dport, m[5] = (uint8_t)(dport >> 8);
	memcpy(m + 6, group, 8);
	memcpy(m + DATA_HEAD, MP(A3), (size_t)len);
	const uint8_t *to = MP(A1);
	if (!in_group) DONE(0);
	if (!memcmp(to, BROADCAST, 6)) {
		for (int i = 0; i < PEER_MAX; i++)
			if (same_group(&peers[i])) plat_net_send(peers[i].mac, m, DATA_HEAD + len);
	} else if (!memcmp(to, my_mac(), 6)) {
		// To itself: the PSP delivers it to its own socket. The game counts on it: its round
		// update sends the player's state to every player slot, its own included, and the
		// scoreboard's row for this player is what comes back (FUN_088c45c0, message 0x23).
		on_data(my_mac(), m, DATA_HEAD + len);
	} else {
		plat_net_send(to, m, DATA_HEAD + len);
	}
	RET(0);
}

// A receive round of the game: it reads the datagrams into one slot per player (a later one
// replaces an earlier one), then handles the slots. Inside a round each sender's datagrams come one
// per round, not one per frame: a round could straddle two frames and lose the first datagram.
void net_recv_round(bool begin) {
	if (begin) round_no = round_no ? round_no + 1 : 1ull << 63;
	in_round = begin;
}

// (id, source MAC out, port out, buffer, length in/out, timeout us, nonblock)
// At most one datagram per sender per frame (or per receive round, above): over the internet a
// sender's datagrams arrive in bursts, where a PSP's come about one per frame; and the game keeps
// only the latest datagram of each player until its next frame (FUN_08b03eb4 into the player's
// slot), so a second one in the same frame would wipe the first. The sender's next datagram waits
// for the next frame.
void hle_sceNetAdhocPdpRecv(CPU *c) {
	int s = sock_index(A0);
	if (s < 0) DONE(ERR_INVALID_SOCKET);
	uint16_t port = socks[s].port;
	uint32_t timeout = A5, nonblock = A6;
	uint64_t end = rt_now_us() + timeout;
	for (;;) {
		net_pump();
		uint64_t frame = in_round ? round_no : rt_vblank_count() + 1;
		for (int k = 0; k < q_len; k++) {
			Packet *q = &queue[(q_first + k) % QUEUE_MAX];
			if (q->dport != port) continue;
			Peer *from = peer_find(q->src);
			bool held = false;  // an earlier datagram of this sender is still queued, or one went this frame
			for (int j = 0; j < k && !held; j++) {
				Packet *e = &queue[(q_first + j) % QUEUE_MAX];
				held = e->dport == port && !memcmp(e->src, q->src, 6);
			}
			if (held || (from && from->taken_at == frame)) continue;
			if ((int)M32(A4) < q->len) {
				M32(A4) = (uint32_t)q->len;
				DONE(ERR_NOT_ENOUGH_SPACE);
			}
			if (from) from->taken_at = frame;
			if (A1) memcpy(MP(A1), q->src, 6);
			if (A2) M16(A2) = q->sport;
			memcpy(MP(A3), q->data, (size_t)q->len);
			M32(A4) = (uint32_t)q->len;
			for (int j = k; j + 1 < q_len; j++) queue[(q_first + j) % QUEUE_MAX] = queue[(q_first + j + 1) % QUEUE_MAX];
			q_len--;
			DONE(0);
		}
		if (nonblock) DONE(ERR_WOULD_BLOCK);
		if (timeout && rt_now_us() >= end) DONE(ERR_TIMEOUT);
		rt_wait_until(rt_now_us() + 2000);
	}
}

void hle_sceNetAdhocGetSocketAlert(CPU *c) {
	if (A1) M32(A1) = 0;
	RET(0);
}

// ---- the mods' own messages --------------------------------------------------------------------
// Messages the game doesn't know about, between the copies in the same group (the game's room), in
// their order of sending; a copy keeps them until a mod takes them.
void net_mod_send(int kind, const void *data, int len) {
	if (!in_group || len < 0 || len > MOD_MAX || plat_net_state() != NET_UP) return;
	static uint8_t m[MOD_HEAD + MOD_MAX];
	m[0] = MSG_MOD;
	m[1] = (uint8_t)kind;
	memcpy(m + 2, group, 8);
	memcpy(m + MOD_HEAD, data, (size_t)len);
	for (int i = 0; i < PEER_MAX; i++)
		if (same_group(&peers[i])) plat_net_send(peers[i].mac, m, MOD_HEAD + len);
}

int net_mod_recv(int kind, uint8_t from[6], void *buf, int cap) {
	net_pump();
	for (int i = 0; i < mq_len; i++) {
		if (modq[i].kind != kind) continue;
		int n = modq[i].len < cap ? modq[i].len : cap;
		if (from) memcpy(from, modq[i].from, 6);
		memcpy(buf, modq[i].data, (size_t)n);
		memmove(&modq[i], &modq[i + 1], sizeof modq[0] * (size_t)(mq_len - i - 1));
		mq_len--;
		return n;
	}
	return 0;
}
