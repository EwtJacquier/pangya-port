// sceUtility: system parameters and the savedata utility (files under ms0:/PSP/SAVEDATA).
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hle.h"
#include "platform.h"
#include "rt.h"

void hle_sceUtilityLoadModule(CPU *c) { RET(0); }
void hle_sceUtilityUnloadModule(CPU *c) { RET(0); }

void hle_sceUtilityGetSystemParamInt(CPU *c) {
	uint32_t v;
	switch (A0) {
	case 2: v = 0; break;    // adhoc channel: automatic
	case 3: v = 0; break;    // wlan power save
	case 4: v = 1; break;    // date format MM/DD/YYYY
	case 5: v = 0; break;    // 24h clock
	case 6: v = 0; break;    // timezone offset (minutes)
	case 7: v = 0; break;    // daylight saving
	case 8: v = 1; break;    // language: English
	case 9: v = 1; break;    // confirm button: cross (US)
	default:
		RET(0x80110103u);
		return;
	}
	M32(A1) = v;
	RET(0);
}

void hle_sceUtilityGetSystemParamString(CPU *c) {
	if (A0 == 1) {  // nickname
		const char *nick = getenv("PANGYA_NICK");
		rt_write_cstr(A1, A2, nick ? nick : "Player");
		RET(0);
		return;
	}
	RET(0x80110103u);
}

// ---- savedata ---------------------------------------------------------------------------------
// SceUtilitySavedataParam offsets (PSP SDK layout).
enum {
	SD_RESULT = 0x1C, SD_MODE = 0x30, SD_GAMENAME = 0x3C, SD_SAVENAME = 0x4C, SD_FILENAME = 0x64,
	SD_DATABUF = 0x74, SD_DATABUFSIZE = 0x78, SD_DATASIZE = 0x7C,
};
enum { ST_NONE = 0, ST_INIT = 1, ST_RUNNING = 2, ST_FINISHED = 3, ST_SHUTDOWN = 4 };
static int sd_status = ST_NONE;
static uint32_t sd_param;

static void save_dir(char *out, size_t cap) {
	const char *ms0 = getenv("PANGYA_MS0");
	snprintf(out, cap, "%s/PSP/SAVEDATA/%.13s%.20s", ms0 ? ms0 : "build/ms0", rt_str(sd_param + SD_GAMENAME),
	         rt_str(sd_param + SD_SAVENAME));
}

static uint32_t savedata_run(void) {
	uint32_t p = sd_param;
	uint32_t mode = M32(p + SD_MODE);
	char dir[700], file[800];
	save_dir(dir, sizeof dir);
	snprintf(file, sizeof file, "%s/%.13s", dir, rt_str(p + SD_FILENAME));
	rt_log("savedata mode=%d game=%s save=%s file=%s", mode, rt_str(p + SD_GAMENAME), rt_str(p + SD_SAVENAME),
	       rt_str(p + SD_FILENAME));
	switch (mode) {
	case 0: case 2: case 4: {  // (auto/list) load
		FILE *f = fopen(file, "rb");
		if (!f) {
			return 0x80110307u;  // load: no data
		}
		size_t n = fread(MP(M32(p + SD_DATABUF)), 1, M32(p + SD_DATABUFSIZE), f);
		fclose(f);
		M32(p + SD_DATASIZE) = (uint32_t)n;
		return 0;
	}
	case 1: case 3: case 5: {  // (auto/list) save
		mkdir(dir, 0755);
		FILE *f = fopen(file, "wb");
		if (!f) {
			return 0x80110385u;
		}
		fwrite(MP(M32(p + SD_DATABUF)), 1, M32(p + SD_DATASIZE), f);
		fclose(f);
		plat_persist();
		return 0;
	}
	default:
		rt_log("savedata mode %d not implemented yet", mode);
		return 0;
	}
}

void hle_sceUtilitySavedataInitStart(CPU *c) {
	sd_param = A0;
	sd_status = ST_RUNNING;
	RET(0);
}
void hle_sceUtilitySavedataGetStatus(CPU *c) {
	int s = sd_status;
	if (sd_status == ST_SHUTDOWN) {
		sd_status = ST_NONE;
	}
	RET(s);
}
void hle_sceUtilitySavedataUpdate(CPU *c) {
	if (sd_status == ST_RUNNING) {
		M32(sd_param + SD_RESULT) = savedata_run();
		sd_status = ST_FINISHED;
	}
	RET(0);
}
void hle_sceUtilitySavedataShutdownStart(CPU *c) {
	sd_status = ST_SHUTDOWN;
	RET(0);
}
