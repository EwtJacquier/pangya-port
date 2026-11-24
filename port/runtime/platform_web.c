// Browser platform (Emscripten): WebGL2 on <canvas id="canvas">, keyboard + Gamepad API,
// WebAudio output (web/audio.js), IndexedDB-backed save files.
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <stdio.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include "platform.h"
#include "rt.h"

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE glctx;

// Input. The page may preset Module.pangyaInput.keyMap (KeyboardEvent.code -> PSP button mask),
// .keyAnalog (code -> [dx, dy] of the PSP analog stick), .padMap (standard-gamepad button index
// -> mask) and .padStick (the PSP analog stick: 0 the left one, 2 the right one; the other one
// works as the d-pad) from the player's settings; the menu edits them live. Module.pangyaMenuOpen keeps keys and buttons away from the game.
EM_JS(void, js_input_init, (), {
	const P = Module.pangyaInput || (Module.pangyaInput = {});
	P.keys = new Set();
	P.downAt = new Map();  // key -> press time: a tap counts for at least 80 ms
	if (!P.keyMap) P.keyMap = {ArrowUp: 0x10, ArrowRight: 0x20, ArrowDown: 0x40, ArrowLeft: 0x80, KeyZ: 0x4000,
	                           KeyX: 0x2000, KeyA: 0x8000, KeyS: 0x1000, KeyQ: 0x100, KeyW: 0x200, Enter: 0x8,
	                           Backspace: 0x1};
	// Standard layout: 0 bottom, 1 right, 2 left, 3 top, 4 LB, 5 RB, 8 back, 9 start, 12-15 d-pad.
	if (!P.padMap) P.padMap = [0x4000, 0x2000, 0x8000, 0x1000, 0x100, 0x200, 0, 0, 0x1, 0x8, 0, 0, 0x10, 0x40, 0x80, 0x20];
	const resumeAudio = () => { if (Module.pangyaAudio && Module.pangyaAudio.state !== 'running') Module.pangyaAudio.resume(); };
	window.addEventListener('keydown', e => {
		resumeAudio();
		if (Module.pangyaMenuOpen || !(P.keyMap[e.code] || (P.keyAnalog && P.keyAnalog[e.code]))) return;
		P.keys.add(e.code);
		P.downAt.set(e.code, performance.now());
		if (!e.ctrlKey && !e.metaKey && !e.altKey) e.preventDefault();
	});
	window.addEventListener('keyup', e => P.keys.delete(e.code));
	window.addEventListener('keydown', e => {  // F9: capture the next frame's GE draws for debugging
		if (e.code !== 'F9') return;
		e.preventDefault();
		Module.pangyaDumpReq = 1;
		console.log('debug capture requested');
	});
	window.addEventListener('pointerdown', resumeAudio);
	window.addEventListener('blur', () => P.keys.clear());
});

EM_JS(int, js_poll_buttons, (uint8_t *axes), {
	const P = Module.pangyaInput;
	let b = 0, ax = 128, ay = 128;
	if (!Module.pangyaMenuOpen) {
		let dx = 0, dy = 0;
		for (const c of P.keys) {
			b |= P.keyMap[c] || 0;
			const a = P.keyAnalog && P.keyAnalog[c];
			if (a) { dx += a[0]; dy += a[1]; }
		}
		if (dx) ax = dx < 0 ? 0 : 255;
		if (dy) ay = dy < 0 ? 0 : 255;
		const now = performance.now();
		for (const [c, t] of P.downAt) {
			if (now - t < 80) b |= P.keyMap[c] || 0;
			else if (!P.keys.has(c)) P.downAt.delete(c);
		}
		const pads = navigator.getGamepads ? navigator.getGamepads() : [];
		for (const p of pads) {
			if (!p) continue;
			p.buttons.forEach((btn, i) => { if (btn.pressed && P.padMap[i]) b |= P.padMap[i]; });
			const s = P.padStick === 0 ? 0 : 2;
			if (p.axes.length >= s + 2) {
				const x = p.axes[s], y = p.axes[s + 1];
				if (Math.hypot(x, y) > 0.15) {  // dead zone: a resting stick must not drift the spin
					ax = Math.max(0, Math.min(255, Math.round((x + 1) * 127.5)));
					ay = Math.max(0, Math.min(255, Math.round((y + 1) * 127.5)));
				}
			}
			const d = s === 2 ? 0 : 2;  // the other stick works as the d-pad (PSP up 0x10, right 0x20, down 0x40, left 0x80)
			if (p.axes.length >= d + 2) {
				const x = p.axes[d], y = p.axes[d + 1];
				if (x < -0.5) b |= 0x80; else if (x > 0.5) b |= 0x20;
				if (y < -0.5) b |= 0x10; else if (y > 0.5) b |= 0x40;
			}
			break;
		}
	}
	else {
		P.keys.clear();
		P.downAt.clear();
	}
	HEAPU8[axes] = ax;
	HEAPU8[axes + 1] = ay;
	return b;
});

// Settings changed in the page's menu (Module.pangyaLive = {scale, uiFilter, dirty}).
EM_JS(int, js_poll_settings, (int *scale, int *filter), {
	const s = Module.pangyaLive;
	if (!s || !s.dirty) return 0;
	s.dirty = false;
	HEAP32[scale >> 2] = s.scale | 0;
	HEAP32[filter >> 2] = s.uiFilter | 0;
	return 1;
});

bool plat_poll_settings(int *scale, int *ui_filter) { return js_poll_settings(scale, ui_filter); }

// The text drawing switches of the page's menu (Module.pangyaText = {strings, ui, dirty}).
EM_JS(int, js_poll_text, (int *strings, int *ui), {
	const s = Module.pangyaText;
	if (!s || !s.dirty) return 0;
	s.dirty = false;
	HEAP32[strings >> 2] = s.strings ? 1 : 0;
	HEAP32[ui >> 2] = s.ui ? 1 : 0;
	return 1;
});

bool plat_poll_text(int *strings, int *ui) { return js_poll_text(strings, ui); }

// The HD textures switch of the page's menu (Module.pangyaHd = {on, dirty}).
EM_JS(int, js_poll_hd, (int *on), {
	const s = Module.pangyaHd;
	if (!s || !s.dirty) return 0;
	s.dirty = false;
	HEAP32[on >> 2] = s.on ? 1 : 0;
	return 1;
});

bool plat_poll_hd(int *on) { return js_poll_hd(on); }

// The filter for the game's own textures (Module.pangyaTexFilter = {mode, dirty}).
EM_JS(int, js_poll_texfilter, (int *mode), {
	const s = Module.pangyaTexFilter;
	if (!s || !s.dirty) return 0;
	s.dirty = false;
	HEAP32[mode >> 2] = s.mode | 0;
	return 1;
});

bool plat_poll_texfilter(int *mode) { return js_poll_texfilter(mode); }

// Audio output lives in web/audio.js (AudioWorklet FIFO); chunks are copied out of the heap.
EM_JS(void, js_audio_push, (const int16_t *p, int n), {
	if (Module.pangyaAudioPush) Module.pangyaAudioPush(HEAP16.slice(p >> 1, (p >> 1) + n * 2));
});

void plat_open(bool hidden) {
	(void)hidden;
	EmscriptenWebGLContextAttributes a;
	emscripten_webgl_init_context_attributes(&a);
	a.majorVersion = 2;
	a.minorVersion = 0;
	a.alpha = false;
	a.depth = false;
	a.stencil = false;
	a.antialias = false;
	glctx = emscripten_webgl_create_context("#canvas", &a);
	if (glctx <= 0) rt_fatal("WebGL2 is not available in this browser");
	emscripten_webgl_make_context_current(glctx);
	// The page keeps the canvas's drawing buffer at its on-screen size (web/shell.html).
	js_input_init();
}

void plat_poll(uint32_t *buttons, uint8_t *ax, uint8_t *ay) {
	uint8_t axes[2];
	*buttons = (uint32_t)js_poll_buttons(axes);
	*ax = axes[0];
	*ay = axes[1];
}

void plat_drawable_size(int *w, int *h) { emscripten_get_canvas_element_size("#canvas", w, h); }

// Sleeping hands control back to the browser. Chained setTimeout calls are clamped to >= 4 ms, so
// the wake-up is delivered through a MessageChannel: code resumed from a message task schedules
// its next timer un-nested, keeping guest timers (sound threads, DelayThread) close to 1 ms.
// A hidden page (a background tab, a minimized window, or one another window covers) gets its
// timers about once a second, and its game nearly stops: in a network game the others wait for it
// (a player's shot showed there only once its window was looked at again, after that copy's own
// hole out had played on). In a network game the timer runs in a worker then, whose timers keep
// their pace, and wakes the game through the channel.
EM_ASYNC_JS(void, js_sleep, (int ms), {
	// One channel for every wake-up, each sleep with its own number (a new MessageChannel a sleep
	// cost more than the rest).
	let S = Module.pangyaSleep;
	if (!S) {
		S = Module.pangyaSleep = {ch: new MessageChannel(), id: 0, resolve: null};
		S.wake = (id) => {
			if (id !== S.id || !S.resolve) return;
			const r = S.resolve;
			S.resolve = null;
			r();
		};
		S.ch.port1.onmessage = (e) => S.wake(e.data);
	}
	const id = ++S.id;
	await new Promise((resolve) => {
		S.resolve = resolve;
		if (ms <= 0) {
			S.ch.port2.postMessage(id);
		} else if (document.hidden && Module.pangyaNet && Module.pangyaNet.state === 2) {
			if (!Module.pangyaTimer) {
				const src = 'onmessage = (e) => setTimeout(() => postMessage(e.data[1]), e.data[0]);';
				Module.pangyaTimer = new Worker(URL.createObjectURL(new Blob([src], {type: 'text/javascript'})));
				Module.pangyaTimer.onmessage = (e) => S.wake(e.data);
			}
			Module.pangyaTimer.postMessage([ms, id]);
		} else {
			setTimeout(() => S.ch.port2.postMessage(id), ms);
		}
	});
});

// The browser shows the canvas when control returns to the event loop, so give it a turn every
// presented frame even if guest threads are busy.
void plat_swap(void) { js_sleep(0); }

// Waits under a millisecond hand the browser a turn too (a message through the channel comes back
// in tens of microseconds; the scheduler calls this again until its timer is due): spinning there
// instead kept the page's thread busy and the browser's own work (network messages, input) waiting.
void plat_sleep(uint64_t us) { js_sleep((int)(us / 1000)); }

bool plat_debug_dump_requested(void) {
	return EM_ASM_INT({ const r = Module.pangyaDumpReq | 0; Module.pangyaDumpReq = 0; return r; }) != 0;
}

EM_JS(void, js_debug_upload, (const char *pathp, const char *namep), {
	const path = UTF8ToString(pathp);
	const name = Date.now() + '_' + UTF8ToString(namep);
	let data;
	try {
		data = FS.readFile(path);
	} catch (e) {
		console.error('debug capture: cannot read ' + path);
		return;
	}
	fetch('/debug-dump?name=' + encodeURIComponent(name), {method: 'POST', body: data})
		.then(r => console.log('debug capture uploaded: ' + name + ' (' + r.status + ')'))
		.catch(e => console.error('debug capture upload failed', e));
});

void plat_debug_upload(const char *path, const char *name) { js_debug_upload(path, name); }

// Replacement textures (texpack.c) arrive in the background; the file appears once complete. A
// picture of the pack comes through the page's texture worker (web/shell.html: from this browser's
// cache when it has it, decoded there into the pixels texpack.c reads, "RAW1"), so the game's
// thread doesn't decode it.
EM_JS(void, js_fetch, (const char *urlp, const char *pathp), {
	const url = UTF8ToString(urlp), path = UTF8ToString(pathp);
	const done = (b) => {
		FS.writeFile(path + '.part', b, {canOwn: true});
		FS.rename(path + '.part', path);
	};
	const pack = Module.pangyaTexture && url.startsWith('textures/') && !url.startsWith('textures/font-') && url.endsWith('.qoi');
	const movie = Module.pangyaMovie && url.startsWith('textures/movies/');  // kept in this browser (web/shell.html)
	(pack ? Module.pangyaTexture(url) : movie ? Module.pangyaMovie(url) : fetch(url).then((r) => {
		if (!r.ok) throw new Error('HTTP ' + r.status);
		return r.arrayBuffer().then((b) => new Uint8Array(b));
	})).then(done).catch((e) => {
		console.error('fetch ' + url + ': ' + (e && e.message || e));
		try { FS.writeFile(path + '.failed', ''); } catch (e2) {}
	});
});

void plat_fetch(const char *url, const char *path) { js_fetch(url, path); }

// Saves live in the project folder when the page's server keeps them (tools/serve_web.py:
// /saves/..., the logged-in player's own with their session header), otherwise in this browser's
// IndexedDB (see web/shell.html).
EM_JS(void, js_persist, (), {
	if (!Module.pangyaServerSaves) {
		FS.syncfs(false, err => { if (err) console.error('save sync failed', err); });
		return;
	}
	const sent = Module.pangyaSavesSent || (Module.pangyaSavesSent = {});
	const walk = (dir, rel) => {
		for (const name of FS.readdir(dir)) {
			if (name === '.' || name === '..') continue;
			const path = dir + '/' + name, r = rel ? rel + '/' + name : name, st = FS.stat(path);
			if (FS.isDir(st.mode)) { walk(path, r); continue; }
			const stamp = st.mtime.getTime() + ':' + st.size;
			if (sent[r] === stamp) continue;
			sent[r] = stamp;
			fetch('saves/' + r, {method: 'PUT', body: FS.readFile(path), headers: Module.pangyaSaveHeaders || {}})
				.then(x => { if (!x.ok) throw new Error('HTTP ' + x.status); console.log('saved to the project: ' + r); })
				.catch(e => { delete sent[r]; console.error('save upload failed', r, e); });
		}
	};
	try { walk('/ms0/PSP/SAVEDATA', 'PSP/SAVEDATA'); } catch (e) { console.error('save upload failed', e); }
});

void plat_persist(void) { js_persist(); }

bool plat_audio_open(void) { return EM_ASM_INT({ return Module.pangyaAudioOpen ? Module.pangyaAudioOpen() : 0; }) != 0; }
void plat_audio_push(const int16_t *frames, int n) { js_audio_push(frames, n); }
uint64_t plat_audio_latency_us(void) {
	return (uint64_t)EM_ASM_DOUBLE({ return Module.pangyaAudioLatency ? Module.pangyaAudioLatency() * 1e6 : 0; });
}

// Ad hoc transport: web/net.js (LiveKit). Nodes are LiveKit participants whose identity is their
// MAC in hex; the page fetches a token for it from the server (tools/serve_web.py).
EM_JS(void, js_net_start, (const char *mac, const char *room), {
	if (Module.pangyaNet) Module.pangyaNet.start(UTF8ToString(mac), UTF8ToString(room));
});
EM_JS(void, js_net_stop, (), { if (Module.pangyaNet) Module.pangyaNet.stop(); });
EM_JS(int, js_net_state, (), { return Module.pangyaNet ? Module.pangyaNet.state : -1; });
EM_JS(void, js_net_send, (const char *to, const uint8_t *p, int n), {
	if (Module.pangyaNet) Module.pangyaNet.send(to ? UTF8ToString(to) : null, HEAPU8.slice(p, p + n));
});
EM_JS(int, js_net_recv, (uint8_t *from, uint8_t *buf, int cap), {
	const N = Module.pangyaNet;
	const m = N && N.take();
	if (!m) return 0;
	for (let i = 0; i < 6; i++) HEAPU8[from + i] = parseInt(m.from.substr(2 * i, 2), 16);
	if (!m.data) return -1;
	const n = Math.min(m.data.length, cap);
	HEAPU8.set(m.data.subarray(0, n), buf);
	return n;
});

static void mac_hex(const uint8_t *m, char *s) {
	for (int i = 0; i < 6; i++) snprintf(s + 2 * i, 3, "%02x", m[i]);
}
void plat_net_start(const uint8_t mac[6], const char *room) {
	char s[13];
	mac_hex(mac, s);
	js_net_start(s, room);
}
void plat_net_stop(void) { js_net_stop(); }
int plat_net_state(void) { return js_net_state(); }
void plat_net_send(const uint8_t *to_mac, const void *data, int len) {
	char s[13];
	if (to_mac) mac_hex(to_mac, s);
	js_net_send(to_mac ? s : NULL, data, len);
}
int plat_net_recv(uint8_t from[6], void *buf, int cap) { return js_net_recv(from, buf, cap); }
#endif
