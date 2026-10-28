/* sf3000-bitfont - tiny embedded 8x8 bitmap-font renderer for raw 16bpp
 * pixel buffers on devices with no GPU/GL and no system font (this whole
 * device family has neither).
 *
 * Built 2026-10-13 for Nikwi Deluxe's password-entry screen (the engine
 * had zero text-rendering code anywhere), but written with no Nikwi-
 * specific assumptions - copy this file pair in verbatim for any future
 * SF3500 port that needs on-screen text (HUD, debug overlay, another
 * game's own menu/name-entry screen), same convention as hwdisp.c/
 * hwaudio.c/joykey.c.
 *
 * Deliberately NOT a general text/UI widget system - just the glyph
 * renderer. A caller decides layout, scrolling, input, etc. itself (see
 * Nikwi's own menu.cpp for one real example: cycling a candidate letter
 * with a gamepad and rendering both the committed string and the
 * currently-highlighted candidate with this).
 */
#ifndef SF3000_BITFONT_H
#define SF3000_BITFONT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Draws one printable ASCII character (0x20 ' ' .. 0x7E '~'; anything
 * outside that range draws nothing, including negative/signed-char
 * values - safe to pass raw user input through unchecked) into a 16bpp
 * pixel buffer at (x, y), the glyph's own top-left corner.
 *
 * `stride` is in PIXELS (shorts), not bytes - the same convention this
 * engine's own Transition code already uses for its pitch1/pitch2 locals
 * (screen->pitch/2), so a caller that already has a pixel stride lying
 * around doesn't need to convert either way.
 *
 * `scale` multiplies each 8x8 source pixel into a scale x scale block (1
 * = native 8x8px - too small to read reliably on this device's real
 * panel, confirmed by the first real-hardware attempt at this password
 * screen showing only an undifferentiated dot-count instead of legible
 * letters; 2 or 3 reads clearly, pick based on how much screen space the
 * caller has).
 *
 * `color` is a pre-packed 16bpp pixel value in the CALLER's own buffer
 * format. This function does no color-space work at all - it only ever
 * writes the exact value it's given, so it works unchanged regardless of
 * which 16bpp layout the caller's buffer uses (every raw buffer actually
 * handed to hwdisp_present() on this device is RGB565 - see hwdisp.c's
 * own header comment - but nothing in this file hardcodes that).
 *
 * Clips silently against buf_w/buf_h on all four sides - safe to call
 * with a glyph partially or fully off-buffer. */
void bitfont_draw_char(unsigned short *pixels, int stride, int buf_w, int buf_h,
	int x, int y, int scale, unsigned short color, char ch);

/* Draws a NUL-terminated string left to right starting at (x, y) - each
 * character's own top-left corner advances by (8*scale + spacing) pixels
 * from the previous one. `spacing` is extra pixels between characters (0
 * for tightly packed; a few pixels reads clearer at small scales, try
 * 1-2 first). Characters outside the printable range are skipped but
 * still advance the cursor (so e.g. embedded newlines don't corrupt
 * alignment, they just render as a blank cell - this function does not
 * interpret '\n' specially, it is not a text-layout engine). */
void bitfont_draw_string(unsigned short *pixels, int stride, int buf_w, int buf_h,
	int x, int y, int scale, int spacing, unsigned short color, const char *str);

/* Pixel width a string of `len` characters would occupy at the given
 * scale/spacing, without needing the actual string - useful for
 * centering a fixed-length field (e.g. Nikwi's own 6-letter password). */
int bitfont_string_width(int len, int scale, int spacing);

#ifdef __cplusplus
}
#endif

#endif
