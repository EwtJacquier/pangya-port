// ClientSetting.txt overrides (debug/test aid and base for mods).
//
// FUN_08885fd0(CClientSetting *this, const char *name) parses ClientSetting.txt from the pak.
// Environment overrides applied right after parsing (see re/README.md for the field map):
//   PANGYA_STARTMODE  m_diStartMode   (+0x5bc) 0 lobby, 1 golf, 2 avatar chat, 3 shop, 4 story
//   PANGYA_MAP        m_diMap         (+0x5c0)
//   PANGYA_HOLE       m_diHole        (+0x5c4) 1..18
#include <stdlib.h>

#include "funcs.h"
#include "rt.h"

static void override(uint32_t self, const char *env, uint32_t off) {
	const char *v = getenv(env);
	if (v) {
		M32(self + off) = (uint32_t)strtol(v, NULL, 0);
		rt_log("ClientSetting: %s -> %d", env, (int)M32(self + off));
	}
}

void f_08885fd0(CPU *c) {
	uint32_t self = c->r[4];
	f_08885fd0_orig(c);
	override(self, "PANGYA_STARTMODE", 0x5bc);
	override(self, "PANGYA_MAP", 0x5c0);
	override(self, "PANGYA_HOLE", 0x5c4);
}
