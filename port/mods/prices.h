#pragma once
#include <stdint.h>

#include "rt.h"

// mods/prices.c: cuts the prices of a shop's list just built (`list`: 0x9fc costumes, 0xa04 the rest).
void shop_prices(uint32_t shop, uint32_t list);

// The Papel Shop's balloon with its price ("costinfo") was drawn by mods/autoload.c at x, y.
void prices_sprite_drawn(CPU *c, uint32_t atlas, float x, float y);
