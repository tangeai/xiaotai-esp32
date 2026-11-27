// SPDX-License-Identifier: MIT
#pragma once
/* Shared by the picture scenarios (product_screens.c, product_screens_gallery.c). */

/* Let the scene settle, save the frame as <shot>-<name>.ppm and add a line to index.tsv. */
void product_screens_shoot(const char *name);
