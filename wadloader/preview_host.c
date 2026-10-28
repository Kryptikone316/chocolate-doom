/* Host-side preview only (not shipped): runs wadloader.c with stub display and
 * scripted input, writing the frames named by WL_SHOTS as PPM files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hwdisp.h"
#include "joykey.h"
extern int wl_main(int, char **);
static int frame_no;
static const char *script;  /* one char per frame: u d l r a b . */
static const char *shotdir;
int hwdisp_init(void) { return 0; }
int hwdisp_active(void) { return 1; }
void hwdisp_set_target_aspect(int a, int b) { (void)a; (void)b; }
void hwdisp_set_filter(int n) { (void)n; }
void hwdisp_deinit(void) {}
void hwdisp_present(const void *src, int w, int h, int pitch)
{
	const unsigned short *p = src;
	char name[300];
	FILE *f;
	int i;
	(void)pitch;
	if (shotdir && script && frame_no < (int)strlen(script) && script[frame_no] == 's') {
		snprintf(name, sizeof(name), "%s/shot%03d.ppm", shotdir, frame_no);
		f = fopen(name, "wb");
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (i = 0; i < w * h; i++) {
			unsigned v = p[i];
			fputc(((v >> 11) & 31) * 255 / 31, f);
			fputc(((v >> 5) & 63) * 255 / 63, f);
			fputc((v & 31) * 255 / 31, f);
		}
		fclose(f);
	}
	if (script && frame_no >= (int)strlen(script) - 1)
		exit(0);
}
void joykey_init(void) {}
unsigned int joykey_read(void)
{
	char c = (script && frame_no < (int)strlen(script)) ? script[frame_no] : '.';
	frame_no++;
	switch (c) {
	case 'u': return 1u << JK_UP;
	case 'd': return 1u << JK_DOWN;
	case 'l': return 1u << JK_LEFT;
	case 'r': return 1u << JK_RIGHT;
	case 'a': return 1u << JK_A;
	case 'b': return 1u << JK_B;
	}
	return 0;
}
int main(int argc, char **argv)
{
	script = getenv("WL_SCRIPT");
	shotdir = getenv("WL_SHOTS");
	return wl_main(argc, argv);
}
