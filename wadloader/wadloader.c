/*
 * WAD loader for the Chocolate Doom port (Datafrog SF3500).
 *
 * Runs before the game. Scans the gamedata folder for Doom game files (IWADs)
 * and add-on level files (PWADs), lets the player choose one of each with the
 * gamepad, and saves the choice to wad.txt, which run.sh passes to the game.
 *
 * Exit status: 0 = go on and start the game, 10 = leave the port (Exit button,
 * or Select + Start). With nothing to choose from (only DOOM1.WAD, no add-ons)
 * it exits 0 straight away without drawing anything.
 *
 * Usage: wadloader [gamedata-folder] [state-file]
 *
 * Display, input and font code are the same small modules the other SF3500
 * ports use (hwdisp.c, joykey.c, bitfont.c).
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bitfont.h"
#include "hwdisp.h"
#include "joykey.h"

#define W 640
#define H 480
#define RGB(r, g, b) ((unsigned short)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

#define EXIT_START 0
#define EXIT_LEAVE 10

enum { FAM_DOOM1 = 1, FAM_DOOM2 = 2 };
enum { MAPS_NONE = 0, MAPS_EXMY = 1, MAPS_MAPXX = 2 };

/* Game files the engine accepts for -iwad, in the order it prefers them. */
static const struct { const char *file; const char *label; int family; } known[] = {
	{ "doom2.wad",     "Doom II",            FAM_DOOM2 },
	{ "plutonia.wad",  "Plutonia",           FAM_DOOM2 },
	{ "tnt.wad",       "TNT: Evilution",     FAM_DOOM2 },
	{ "doom.wad",      "Doom",               FAM_DOOM1 },
	{ "doom1.wad",     "Doom Shareware",     FAM_DOOM1 },
	{ "doom2f.wad",    "Doom II (French)",   FAM_DOOM2 },
	{ "chex.wad",      "Chex Quest",         FAM_DOOM1 },
	{ "hacx.wad",      "HacX",               FAM_DOOM2 },
	{ "freedoom2.wad", "Freedoom: Phase 2",  FAM_DOOM2 },
	{ "freedoom1.wad", "Freedoom: Phase 1",  FAM_DOOM1 },
	{ "freedm.wad",    "FreeDM",             FAM_DOOM2 },
};
#define NKNOWN ((int)(sizeof(known) / sizeof(known[0])))

typedef struct {
	char name[64];
	int known_index;  /* IWADs: index into known[]; PWADs: -1 */
	int family;       /* IWADs */
	int maps;         /* PWADs: MAPS_* */
} wad_t;

#define MAXWADS 64
static wad_t iwads[MAXWADS], pwads[MAXWADS];
static int n_iwads, n_pwads;

static unsigned short fb[W * H];

/* ---------- scanning ---------- */

static int safe_name(const char *n)
{
	int i;
	for (i = 0; n[i]; i++) {
		if (!(isalnum((unsigned char)n[i]) || n[i] == '.' || n[i] == '_' || n[i] == '-'))
			return 0;
	}
	return i > 4 && i < 60;
}

static int ends_wad(const char *n)
{
	size_t l = strlen(n);
	return l > 4 && strcasecmp(n + l - 4, ".wad") == 0;
}

static unsigned le32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static int classify_maps(FILE *f, unsigned numlumps, unsigned infoofs)
{
	unsigned char e[16 * 64];
	unsigned i, left = numlumps, got = 0;
	int found = MAPS_NONE;

	if (numlumps > 20000)
		numlumps = left = 20000;
	if (fseek(f, (long)infoofs, SEEK_SET) != 0)
		return MAPS_NONE;
	while (left && found == MAPS_NONE) {
		unsigned chunk = left > 64 ? 64 : left;
		if (fread(e, 16, chunk, f) != chunk)
			break;
		for (i = 0; i < chunk; i++) {
			const unsigned char *n = e + i * 16 + 8;
			if (n[0] == 'M' && n[1] == 'A' && n[2] == 'P' && isdigit(n[3]) &&
			    isdigit(n[4]) && n[5] == 0) {
				found = MAPS_MAPXX;
				break;
			}
			if (n[0] == 'E' && isdigit(n[1]) && n[2] == 'M' && isdigit(n[3]) && n[4] == 0) {
				found = MAPS_EXMY;
				break;
			}
		}
		left -= chunk;
		got += chunk;
	}
	return found;
}

static int cmp_wad(const void *a, const void *b)
{
	return strcasecmp(((const wad_t *)a)->name, ((const wad_t *)b)->name);
}

static int cmp_iwad(const void *a, const void *b)
{
	return ((const wad_t *)a)->known_index - ((const wad_t *)b)->known_index;
}

static void scan(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	if (!d)
		return;
	while ((de = readdir(d)) != NULL) {
		char path[512], lower[64];
		unsigned char hdr[12];
		struct stat st;
		FILE *f;
		unsigned numlumps, infoofs;
		int k, i;

		if (!ends_wad(de->d_name) || !safe_name(de->d_name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 12)
			continue;
		f = fopen(path, "rb");
		if (!f)
			continue;
		if (fread(hdr, 1, 12, f) != 12) {
			fclose(f);
			continue;
		}
		numlumps = le32(hdr + 4);
		infoofs = le32(hdr + 8);
		if ((memcmp(hdr, "IWAD", 4) != 0 && memcmp(hdr, "PWAD", 4) != 0) ||
		    numlumps > 200000 || (unsigned long long)infoofs + 16ull * numlumps > (unsigned long long)st.st_size) {
			fclose(f);
			continue;
		}
		for (i = 0; de->d_name[i] && i < 63; i++)
			lower[i] = tolower((unsigned char)de->d_name[i]);
		lower[i] = 0;
		k = -1;
		for (i = 0; i < NKNOWN; i++)
			if (strcmp(lower, known[i].file) == 0)
				k = i;

		if (memcmp(hdr, "IWAD", 4) == 0) {
			if (k >= 0 && n_iwads < MAXWADS) {
				snprintf(iwads[n_iwads].name, sizeof(iwads[0].name), "%s", de->d_name);
				iwads[n_iwads].known_index = k;
				iwads[n_iwads].family = known[k].family;
				n_iwads++;
			}
		} else if (k < 0 && n_pwads < MAXWADS) {
			snprintf(pwads[n_pwads].name, sizeof(pwads[0].name), "%s", de->d_name);
			pwads[n_pwads].known_index = -1;
			pwads[n_pwads].maps = classify_maps(f, numlumps, infoofs);
			n_pwads++;
		}
		fclose(f);
	}
	closedir(d);
	qsort(iwads, n_iwads, sizeof(wad_t), cmp_iwad);
	qsort(pwads, n_pwads, sizeof(wad_t), cmp_wad);
}

/* ---------- saved choice ---------- */

static int find_name(const wad_t *list, int n, const char *name)
{
	int i;
	for (i = 0; i < n; i++)
		if (strcasecmp(list[i].name, name) == 0)
			return i;
	return -1;
}

static void load_state(const char *file, int *sel_iwad, int *sel_pwad)
{
	FILE *f = fopen(file, "r");
	char line[256], val[128];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "iwad=\"%127[^\"]\"", val) == 1) {
			int i = find_name(iwads, n_iwads, val);
			if (i >= 0)
				*sel_iwad = i;
		} else if (sscanf(line, "pwad=\"%127[^\"]\"", val) == 1) {
			int i = find_name(pwads, n_pwads, val);
			if (i >= 0)
				*sel_pwad = i;
		}
	}
	fclose(f);
}

static int save_state(const char *file, int sel_iwad, int sel_pwad)
{
	char tmp[300];
	FILE *f;
	snprintf(tmp, sizeof(tmp), "%s.tmp", file);
	f = fopen(tmp, "w");
	if (!f)
		return 0;
	fprintf(f, "# Written by the WAD loader. You can also edit it by hand.\n");
	fprintf(f, "# iwad: the game file; pwad: add-on level files, space separated.\n");
	fprintf(f, "iwad=\"%s\"\n", sel_iwad >= 0 ? iwads[sel_iwad].name : "");
	fprintf(f, "pwad=\"%s\"\n", sel_pwad >= 0 ? pwads[sel_pwad].name : "");
	if (fclose(f) != 0)
		return 0;
	return rename(tmp, file) == 0;
}


/* ---------- graphics from the player's own WAD ---------- */

/* The font, logo and skull cursor are read from the game file at run time, so
 * the loader looks like the game and nothing from it is copied or shipped.
 * If anything is missing the built-in font is used instead. */
typedef struct { int w, h, ox, oy; unsigned short *px; unsigned char *op; } patch_t;

static unsigned char palette[256][3];
static int have_palette;
static patch_t font[96];           /* index = ASCII code - 32; w == 0: no glyph */
static patch_t logo, skull[2];
static int have_font;

static unsigned char *read_lump(FILE *f, const unsigned char *dirent, long *size_out)
{
	unsigned pos = le32(dirent), size = le32(dirent + 4);
	unsigned char *buf;
	if (size == 0 || size > (1u << 22) || fseek(f, (long)pos, SEEK_SET) != 0)
		return NULL;
	buf = malloc(size);
	if (buf && fread(buf, 1, size, f) != size) {
		free(buf);
		return NULL;
	}
	*size_out = (long)size;
	return buf;
}

static int decode_patch(const unsigned char *d, long size, patch_t *p)
{
	int w, h, x;
	if (size < 8)
		return 0;
	w = d[0] | (d[1] << 8);
	h = d[2] | (d[3] << 8);
	if (w <= 0 || h <= 0 || w > 400 || h > 250 || size < 8 + 4 * w)
		return 0;
	p->w = w;
	p->h = h;
	p->ox = (short)(d[4] | (d[5] << 8));
	p->oy = (short)(d[6] | (d[7] << 8));
	p->px = calloc((size_t)w * h, sizeof(unsigned short));
	p->op = calloc((size_t)w * h, 1);
	if (!p->px || !p->op)
		return 0;
	for (x = 0; x < w; x++) {
		long o = le32(d + 8 + 4 * x);
		while (o < size && d[o] != 0xFF) {
			int top = d[o], len = d[o + 1], i;
			if (o + 3 + len >= size)
				break;
			for (i = 0; i < len; i++) {
				int y = top + i;
				if (y < h) {
					const unsigned char *c = palette[d[o + 3 + i]];
					p->px[y * w + x] = RGB(c[0], c[1], c[2]);
					p->op[y * w + x] = 1;
				}
			}
			o += len + 4;
		}
	}
	return 1;
}

static void load_graphics(const char *wadpath)
{
	FILE *f = fopen(wadpath, "rb");
	unsigned char hdr[12], *dir = NULL, *pal = NULL;
	unsigned n, ofs, i;
	long sz;
	int k;

	if (!f)
		return;
	if (fread(hdr, 1, 12, f) != 12)
		goto done;
	n = le32(hdr + 4);
	ofs = le32(hdr + 8);
	if (n == 0 || n > 200000)
		goto done;
	dir = malloc((size_t)n * 16);
	if (!dir || fseek(f, (long)ofs, SEEK_SET) != 0 || fread(dir, 16, n, f) != n)
		goto done;

#define FIND(name, out)                                                           \
	do {                                                                      \
		out = NULL;                                                       \
		for (i = 0; i < n; i++)                                           \
			if (strncmp((const char *)dir + i * 16 + 8, name, 8) == 0) {  \
				out = dir + i * 16;                                       \
				break;                                                    \
			}                                                                 \
	} while (0)

	{
		unsigned char *e;
		FIND("PLAYPAL", e);
		if (!e || !(pal = read_lump(f, e, &sz)) || sz < 768)
			goto done;
		for (k = 0; k < 256; k++) {
			palette[k][0] = pal[k * 3];
			palette[k][1] = pal[k * 3 + 1];
			palette[k][2] = pal[k * 3 + 2];
		}
		have_palette = 1;
		for (k = 33; k <= 95; k++) {
			char name[9];
			unsigned char *g;
			snprintf(name, sizeof(name), "STCFN%03d", k);
			FIND(name, e);
			if (e && (g = read_lump(f, e, &sz))) {
				decode_patch(g, sz, &font[k - 32]);
				free(g);
			}
		}
		have_font = font['A' - 32].w > 0 && font['O' - 32].w > 0;
		{
			static const char *names[3] = { "M_DOOM", "M_SKULL1", "M_SKULL2" };
			patch_t *dst[3] = { &logo, &skull[0], &skull[1] };
			for (k = 0; k < 3; k++) {
				unsigned char *g;
				FIND(names[k], e);
				if (e && (g = read_lump(f, e, &sz))) {
					decode_patch(g, sz, dst[k]);
					free(g);
				}
			}
		}
	}
done:
	free(pal);
	free(dir);
	fclose(f);
}

/* Draws a patch at (x, y) minus its own offsets (as the game does), scaled by
 * sn/sd with nearest-neighbour sampling. */
static void blit_scaled(const patch_t *p, int x, int y, int sn, int sd)
{
	int dw = p->w * sn / sd, dh = p->h * sn / sd, dx, dy;
	x -= p->ox * sn / sd;
	y -= p->oy * sn / sd;
	for (dy = 0; dy < dh; dy++)
		for (dx = 0; dx < dw; dx++) {
			int px = dx * sd / sn, py = dy * sd / sn, fx = x + dx, fy = y + dy;
			if (px < p->w && py < p->h && p->op[py * p->w + px] &&
			    fx >= 0 && fx < W && fy >= 0 && fy < H)
				fb[fy * W + fx] = p->px[py * p->w + px];
		}
}

static void blit(const patch_t *p, int x, int y, int scale)
{
	blit_scaled(p, x, y, scale, 1);
}

/* Doom's font only has capitals; lower case is shown as capitals too. */
static const patch_t *glyph(char ch)
{
	int c = toupper((unsigned char)ch);
	if (c < 33 || c > 95 || font[c - 32].w == 0)
		return NULL;
	return &font[c - 32];
}

static int doom_text_width(const char *s, int scale)
{
	int w = 0;
	for (; *s; s++) {
		const patch_t *g = glyph(*s);
		w += (g ? g->w : 4) * scale;
	}
	return w;
}

/* Tint: Doom's own font is a flat red-grey, so colours are only used on the
 * fallback font. */
static void doom_text(int x, int y, int scale, const char *s)
{
	for (; *s; s++) {
		const patch_t *g = glyph(*s);
		if (g)
			blit(g, x, y, scale);
		x += (g ? g->w : 4) * scale;
	}
}

/* ---------- drawing ---------- */

static const unsigned short C_BG = RGB(22, 14, 14), C_TITLE = RGB(215, 45, 30),
	C_BOX = RGB(58, 48, 48), C_BOXHI = RGB(96, 70, 60), C_TEXT = RGB(240, 235, 225),
	C_DIM = RGB(160, 150, 140), C_FOCUS = RGB(255, 215, 60), C_ERR = RGB(255, 120, 90);

static void fill(int x, int y, int w, int h, unsigned short c)
{
	int xx, yy;
	for (yy = y; yy < y + h && yy < H; yy++) {
		if (yy < 0)
			continue;
		for (xx = x; xx < x + w && xx < W; xx++)
			if (xx >= 0)
				fb[yy * W + xx] = c;
	}
}

static void frame(int x, int y, int w, int h, int t, unsigned short c)
{
	fill(x, y, w, t, c);
	fill(x, y + h - t, w, t, c);
	fill(x, y, t, h, c);
	fill(x + w - t, y, t, h, c);
}

/* `scale` is for the built-in 8x8 font; Doom's own font is about 8 pixels tall
 * too, so the same number gives a similar size. */
static void text(int x, int y, int scale, unsigned short c, const char *s)
{
	if (have_font)
		doom_text(x, y, scale, s);
	else
		bitfont_draw_string(fb, W, W, H, x, y, scale, 0, c, s);
}

static int text_width(const char *s, int scale)
{
	return have_font ? doom_text_width(s, scale)
	                 : bitfont_string_width((int)strlen(s), scale, 0);
}

static void text_center(int cx, int y, int scale, unsigned short c, const char *s)
{
	text(cx - text_width(s, scale) / 2, y, scale, c, s);
}

static void shorten(char *dst, size_t n, const char *s, int maxchars)
{
	snprintf(dst, n, "%s", s);
	if ((int)strlen(dst) > maxchars) {
		dst[maxchars - 2] = '.';
		dst[maxchars - 1] = '.';
		dst[maxchars] = 0;
	}
}

static const char *iwad_label(int i)
{
	return known[iwads[i].known_index].label;
}

static const char *maps_tag(int m)
{
	return m == MAPS_MAPXX ? "Doom II maps" : m == MAPS_EXMY ? "Doom 1 maps" : "";
}

/* ---------- main ---------- */

enum { ROW_GAME, ROW_ADDON, ROW_BUTTONS, ROW_EXIT, ROWS };
enum { BTN_SET, BTN_CANCEL };

static unsigned buttons_now(void)
{
	return joykey_read();
}

#define BTN(b) (1u << (b))

static void sleep_ms(int ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

static void entry_text(char *out, size_t n, int which, int idx)
{
	char tmp[96];
	if (which == ROW_GAME) {
		snprintf(tmp, sizeof(tmp), "%s  %s", iwads[idx].name, iwad_label(idx));
	} else if (idx < 0) {
		snprintf(tmp, sizeof(tmp), "None");
	} else if (pwads[idx].maps) {
		snprintf(tmp, sizeof(tmp), "%s  %s", pwads[idx].name, maps_tag(pwads[idx].maps));
	} else {
		snprintf(tmp, sizeof(tmp), "%s", pwads[idx].name);
	}
	shorten(out, n, tmp, 31);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "gamedata";
	const char *state = argc > 2 ? argv[2] : "wad.txt";
	int sel_iwad = -1, sel_pwad = -1, saved_iwad, saved_pwad;
	int row = ROW_BUTTONS, col = BTN_SET;
	int open = -1, cursor = 0, top = 0;
	char msg[64] = "", hint[64];
	unsigned prev, hold_since = 0;
	int running = 1, result = EXIT_START;
	unsigned frames = 0, held_dir = 0, held_for = 0;

	scan(dir);
	if (n_iwads == 0 || (n_iwads == 1 && n_pwads == 0))
		return EXIT_START;

	sel_iwad = 0;
	load_state(state, &sel_iwad, &sel_pwad);
	saved_iwad = sel_iwad;
	saved_pwad = sel_pwad;

	{
		char wadpath[600];
		snprintf(wadpath, sizeof(wadpath), "%s/%s", dir, iwads[sel_iwad].name);
		load_graphics(wadpath);
	}

	if (hwdisp_init() != 0 && !hwdisp_active())
		return EXIT_START;
	hwdisp_set_target_aspect(4, 3);
	joykey_init();
	prev = buttons_now();   /* ignore buttons already held at launch */
	(void)hold_since;

	while (running) {
		unsigned now = buttons_now(), pressed = now & ~prev, ev = pressed;
		int i;

		/* Auto-repeat for held up / down. */
		if (now & (BTN(JK_UP) | BTN(JK_DOWN))) {
			unsigned dir_bit = now & (BTN(JK_UP) | BTN(JK_DOWN));
			if (dir_bit != held_dir) {
				held_dir = dir_bit;
				held_for = 0;
			} else if (++held_for > 20 && (held_for % 5) == 0) {
				ev |= dir_bit;
			}
		} else {
			held_dir = 0;
			held_for = 0;
		}
		prev = now;

		if ((now & BTN(JK_SELECT)) && (now & BTN(JK_START)) &&
		    (pressed & (BTN(JK_SELECT) | BTN(JK_START)))) {
			result = EXIT_LEAVE;
			break;
		}

		if (open >= 0) {
			int count = open == ROW_GAME ? n_iwads : n_pwads + 1;
			if (ev & BTN(JK_UP) && cursor > 0)
				cursor--;
			if (ev & BTN(JK_DOWN) && cursor < count - 1)
				cursor++;
			if (cursor < top)
				top = cursor;
			if (cursor > top + 7)
				top = cursor - 7;
			if (pressed & BTN(JK_B))
				open = -1;
			if (pressed & BTN(JK_A)) {
				if (open == ROW_GAME)
					sel_iwad = cursor;
				else
					sel_pwad = cursor - 1;
				msg[0] = 0;
				open = -1;
			}
		} else {
			if (ev & BTN(JK_UP) && row > 0)
				row--;
			if (ev & BTN(JK_DOWN) && row < ROWS - 1)
				row++;
			if (row == ROW_BUTTONS) {
				if (pressed & BTN(JK_LEFT))
					col = BTN_SET;
				if (pressed & BTN(JK_RIGHT))
					col = BTN_CANCEL;
			}
			if (pressed & BTN(JK_A)) {
				if (row == ROW_GAME || row == ROW_ADDON) {
					open = row;
					cursor = row == ROW_GAME ? sel_iwad : sel_pwad + 1;
					top = cursor > 7 ? cursor - 7 : 0;
				} else if (row == ROW_EXIT) {
					result = EXIT_LEAVE;
					break;
				} else if (col == BTN_CANCEL) {
					sel_iwad = saved_iwad;
					sel_pwad = saved_pwad;
					result = EXIT_START;
					break;
				} else {
					int fam = iwads[sel_iwad].family;
					int maps = sel_pwad >= 0 ? pwads[sel_pwad].maps : MAPS_NONE;
					if (maps == MAPS_MAPXX && fam != FAM_DOOM2) {
						snprintf(msg, sizeof(msg), "That add-on needs a Doom II game");
					} else if (maps == MAPS_EXMY && fam != FAM_DOOM1) {
						snprintf(msg, sizeof(msg), "That add-on needs a Doom 1 game");
					} else if (!save_state(state, sel_iwad, sel_pwad)) {
						snprintf(msg, sizeof(msg), "Could not save the choice");
					} else {
						result = EXIT_START;
						break;
					}
				}
			}
		}

		/* ----- draw ----- */
		fill(0, 0, W, H, C_BG);
		if (logo.px)
			blit_scaled(&logo, (W - logo.w * 3 / 2) / 2, 8, 3, 2);
		else
			text_center(W / 2, 30, 3, C_TITLE, "CHOCOLATE DOOM");

		for (i = 0; i < 2; i++) {
			int y = i == 0 ? 112 : 196, focus = (row == i && open < 0);
			char line[64];
			text(40, y, 2, C_TEXT, i == 0 ? "Game" : "Add-on, optional");
			fill(40, y + 26, 560, 40, focus ? C_BOXHI : C_BOX);
			frame(40, y + 26, 560, 40, focus ? 3 : 1, focus ? C_FOCUS : C_DIM);
			entry_text(line, sizeof(line), i == 0 ? ROW_GAME : ROW_ADDON,
			           i == 0 ? sel_iwad : sel_pwad);
			text(52, y + 38, 2, C_TEXT, line);
			text(568, y + 38, 2, C_FOCUS, "v");
		}

		if (msg[0])
			text_center(W / 2, 290, 2, C_ERR, msg);
		else {
			snprintf(hint, sizeof(hint), "%d game%s and %d add-on%s found", n_iwads,
			         n_iwads == 1 ? "" : "s", n_pwads, n_pwads == 1 ? "" : "s");
			text_center(W / 2, 290, 2, C_DIM, hint);
		}

		for (i = 0; i < 2; i++) {
			int x = i == 0 ? 100 : 340, focus = (row == ROW_BUTTONS && col == i && open < 0);
			fill(x, 330, 200, 48, focus ? C_BOXHI : C_BOX);
			frame(x, 330, 200, 48, focus ? 3 : 1, focus ? C_FOCUS : C_DIM);
			text_center(x + 100, 345, 2, C_TEXT, i == 0 ? "Set WAD" : "Cancel");
		}

		{
			int focus = (row == ROW_EXIT && open < 0);
			fill(220, 416, 200, 48, focus ? C_BOXHI : C_BOX);
			frame(220, 416, 200, 48, focus ? 3 : 1, focus ? C_FOCUS : C_DIM);
			text_center(W / 2, 431, 2, C_TEXT, "Exit");
		}

		if (open >= 0) {
			int count = open == ROW_GAME ? n_iwads : n_pwads + 1;
			int rows = count > 8 ? 8 : count;
			int py = 112, ph = rows * 34 + 20;
			fill(30, py, 580, ph, C_BG);
			frame(30, py, 580, ph, 3, C_FOCUS);
			for (i = 0; i < rows; i++) {
				int idx = top + i;
				char line[64];
				if (idx >= count)
					break;
				entry_text(line, sizeof(line), open,
				           open == ROW_GAME ? idx : idx - 1);
				if (idx == cursor)
					fill(40, py + 10 + i * 34, 560, 32, C_BOXHI);
				text(52, py + 17 + i * 34, 2, idx == cursor ? C_FOCUS : C_TEXT, line);
			}
			if (top > 0)
				text(578, py + 12, 2, C_DIM, "^");
			if (top + rows < count)
				text(578, py + ph - 30, 2, C_DIM, "v");
		}

		if (skull[0].px && open < 0) {
			const patch_t *sk = &skull[(frames / 16) & 1 && skull[1].px ? 1 : 0];
			int sx, sy;
			if (row == ROW_GAME || row == ROW_ADDON) {
				sx = 2;
				sy = (row == ROW_GAME ? 112 : 196) + 26 + (40 - sk->h * 2) / 2;
			} else if (row == ROW_EXIT) {
				sx = 220 - sk->w * 2 - 6;
				sy = 416 + (48 - sk->h * 2) / 2;
			} else {
				sx = (col == BTN_SET ? 100 : 340) - sk->w * 2 - 6;
				sy = 330 + (48 - sk->h * 2) / 2;
			}
			blit(sk, sx, sy, 2);
		}

		hwdisp_present(fb, W, H, W * 2);
		frames++;
		sleep_ms(16);
	}

	hwdisp_deinit();
	return result;
}
