#include "sf3500_bridge.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../d_event.h"
#include "../doomkeys.h"
#include "../m_config.h"
#include "../m_controls.h"
#include "hwaudio.h"
#include "hwdisp.h"
#include "joykey.h"

static unsigned int sPrevJoyKey = 0;

static unsigned short *sFrameBuf = NULL;
static int sFrameBufW = 0;
static int sFrameBufH = 0;

static Uint32 *sNativeBuf = NULL;
static int sNativeBufW = 0;
static int sNativeBufH = 0;

void Sf3500Init(void)
{
	hwdisp_init();
	/* Matches the driver's own fixed internal 640x480 (4:3) canvas stage -
	 * see sf3500_portmaster_project.md. */
	hwdisp_set_target_aspect(4, 3);
	joykey_init();
	hwaudio_init();
}

static void Sf3500PostKey(const bool down, const int doomKey)
{
	event_t ev;
	ev.type = down ? ev_keydown : ev_keyup;
	ev.data1 = doomKey;
	ev.data2 = doomKey;
	ev.data3 = 0;
	D_PostEvent(&ev);
}

void Sf3500PumpInput(void)
{
	const unsigned int jk = joykey_read();
	if (jk == sPrevJoyKey)
	{
		return;
	}
	const unsigned int changed = jk ^ sPrevJoyKey;
	sPrevJoyKey = jk;

	// SELECT+START together: bypass the game's own menu/quit path entirely,
	// same standing rule as every prior port on this device.
	if ((jk & (1u << JK_SELECT)) && (jk & (1u << JK_START)))
	{
		/* exit() here skips Doom's own quit path, which is what saves the
		 * config - without this, slider and key changes were lost. */
		M_SaveDefaults();
		Sf3500Deinit();
		exit(0);
	}

	// Doom's own default keyboard bindings already map cleanly onto a
	// d-pad + 6 buttons - no config changes needed. Posting ev_keydown/
	// ev_keyup directly bypasses SDL's joystick/GameController layer
	// entirely (confirmed broken for virtual joysticks on this device).
	if (changed & (1u << JK_UP)) Sf3500PostKey(jk & (1u << JK_UP), KEY_UPARROW);
	if (changed & (1u << JK_DOWN)) Sf3500PostKey(jk & (1u << JK_DOWN), KEY_DOWNARROW);
	if (changed & (1u << JK_LEFT)) Sf3500PostKey(jk & (1u << JK_LEFT), KEY_LEFTARROW);
	if (changed & (1u << JK_RIGHT)) Sf3500PostKey(jk & (1u << JK_RIGHT), KEY_RIGHTARROW);
	// A/B are dual-mapped: the menu's "select"/"back" keys (key_menu_forward
	// = KEY_ENTER, key_menu_back = KEY_BACKSPACE, m_controls.c) are
	// completely separate from the in-game fire/use keys - sending both on
	// the same button press is safe and context-free, since ENTER/
	// BACKSPACE do nothing during gameplay and RCTRL/space do nothing in a
	// menu.
	if (changed & (1u << JK_A))
	{
		Sf3500PostKey(jk & (1u << JK_A), KEY_RCTRL); // fire
		Sf3500PostKey(jk & (1u << JK_A), KEY_ENTER); // menu select
	}
	if (changed & (1u << JK_B))
	{
		Sf3500PostKey(jk & (1u << JK_B), ' '); // use
		Sf3500PostKey(jk & (1u << JK_B), KEY_BACKSPACE); // menu back
	}
	if (changed & (1u << JK_X)) Sf3500PostKey(jk & (1u << JK_X), KEY_RALT); // strafe
	if (changed & (1u << JK_Y)) Sf3500PostKey(jk & (1u << JK_Y), KEY_RSHIFT); // speed
	// L1/R1 were unused - Doom's Y/N confirmation dialogs (quit, end game,
	// quickload/quicksave overwrite) need key_menu_abort='n'/key_menu_confirm
	// ='y' (m_controls.c), separate from both the menu select/back keys
	// (A/B above) and gameplay keys - nothing else to conflict with, since
	// 'y'/'n' do nothing outside those dialogs.
	/* L1 / R1: "No" / "Yes" in menu prompts, and previous / next weapon in
	 * the game (the number keys are not on a gamepad). The engine steps
	 * through the weapons the player owns and wraps round; its keys are
	 * unbound by default, so give them values once the config is loaded. */
	if (key_prevweapon == 0)
		key_prevweapon = '[';
	if (key_nextweapon == 0)
		key_nextweapon = ']';
	if (changed & (1u << JK_L1))
	{
		Sf3500PostKey(jk & (1u << JK_L1), 'n'); // menu abort/no
		Sf3500PostKey(jk & (1u << JK_L1), key_prevweapon);
	}
	if (changed & (1u << JK_R1))
	{
		Sf3500PostKey(jk & (1u << JK_R1), 'y'); // menu confirm/yes
		Sf3500PostKey(jk & (1u << JK_R1), key_nextweapon);
	}
	if (changed & (1u << JK_START)) Sf3500PostKey(jk & (1u << JK_START), KEY_ESCAPE); // menu
	if (changed & (1u << JK_SELECT)) Sf3500PostKey(jk & (1u << JK_SELECT), KEY_TAB); // map
}

static short *sUpsampleBuf = NULL;
static int sUpsampleBufFrames = 0;

/* Substitute-music playback, streamed directly from disk and mixed in
 * here - NOT via SDL_mixer's Mix_Chunk/Mix_PlayChannel. Root cause,
 * confirmed via real per-call timing instrumentation (2026-10-10): SDL's
 * own audio-generation callback on this device fires at a fraction of
 * the real-time rate its opened spec implies (measured ~11225Hz
 * effective vs the 24000Hz asked for, over a 91-second real window,
 * confirmed stable, not noise). SDL_mixer's playback-position tracking
 * is driven entirely by callback invocation count, not a real-time
 * clock - so the SONG ITSELF advances at that same reduced fraction of
 * real speed, regardless of anything done on the output side. Every
 * prior fix (OGG/WAV, fixed ratios, real-time-adaptive stretching) was
 * reshaping content SDL had already generated too slowly - none of them
 * could possibly have worked, since the deficit is in SDL's own content
 * generation, not in how we relay it. hwaudio's own delivery-to-hardware
 * pacing was separately confirmed correct (hwaudiodiag.log: ~10ms wake
 * interval, ~38us play() calls, healthy ring) - this is the one place
 * left where the bug can be. Streaming (not loading fully into RAM,
 * same OOM risk Mix_Chunk had) and reading exactly `out_frames` worth of
 * PCM per call - the SAME real-time-computed count already used for the
 * SFX stretch below - means music advances by genuine real elapsed
 * time, fully independent of SDL's callback cadence. */
static FILE *sMusicFile = NULL;
static long sMusicDataOffset = 0;
static long sMusicDataBytes = 0;
static long sMusicPos = 0;
static bool sMusicPlaying = false;
static bool sMusicLooping = false;
static int sMusicVolume = 128;

static short *sMusicReadBuf = NULL;
static int sMusicReadBufFrames = 0;

int Sf3500MusicLoad(const char *path)
{
	if (sMusicFile != NULL)
	{
		fclose(sMusicFile);
		sMusicFile = NULL;
	}
	sMusicPlaying = false;

	FILE *f = fopen(path, "rb");
	if (f == NULL)
	{
		return 0;
	}

	char riffHeader[12];
	if (fread(riffHeader, 1, 12, f) != 12
	 || memcmp(riffHeader, "RIFF", 4) != 0
	 || memcmp(riffHeader + 8, "WAVE", 4) != 0)
	{
		fclose(f);
		return 0;
	}

	long dataOffset = -1;
	long dataBytes = 0;
	for (;;)
	{
		char chunkId[4];
		unsigned int chunkSize;
		if (fread(chunkId, 1, 4, f) != 4) break;
		if (fread(&chunkSize, 4, 1, f) != 1) break;
		if (memcmp(chunkId, "data", 4) == 0)
		{
			dataOffset = ftell(f);
			dataBytes = (long)chunkSize;
			break;
		}
		if (fseek(f, (long)chunkSize + (long)(chunkSize & 1), SEEK_CUR) != 0)
		{
			break;
		}
	}

	if (dataOffset < 0)
	{
		fclose(f);
		return 0;
	}

	sMusicFile = f;
	sMusicDataOffset = dataOffset;
	sMusicDataBytes = dataBytes;
	sMusicPos = 0;
	return 1;
}

void Sf3500MusicPlay(int loop)
{
	if (sMusicFile == NULL)
	{
		return;
	}
	fseek(sMusicFile, sMusicDataOffset, SEEK_SET);
	sMusicPos = 0;
	sMusicLooping = loop ? true : false;
	sMusicPlaying = true;
}

void Sf3500MusicStop(void)
{
	sMusicPlaying = false;
}

void Sf3500MusicSetVolume(int volume_0_to_128)
{
	if (volume_0_to_128 < 0) volume_0_to_128 = 0;
	if (volume_0_to_128 > 128) volume_0_to_128 = 128;
	sMusicVolume = volume_0_to_128;
}

int Sf3500MusicIsPlaying(void)
{
	return sMusicPlaying ? 1 : 0;
}

/* Reads exactly `frames` worth of PCM (looping/stopping at EOF as
 * appropriate) and additively mixes it into buf, clipped to 16-bit
 * range. buf is already real 48000Hz stereo content (the SFX-stretch
 * output) - our WAV files are themselves already rendered at 48000Hz
 * stereo (see chocolate_doom_handoff.md), so this is a direct 1:1 mix,
 * no resampling needed on the music side at all. */
static void Sf3500MusicMixInto(short *buf, int frames)
{
	if (!sMusicPlaying || sMusicFile == NULL)
	{
		return;
	}

	if (frames > sMusicReadBufFrames)
	{
		free(sMusicReadBuf);
		sMusicReadBuf = malloc((size_t)frames * 2 * sizeof(short));
		sMusicReadBufFrames = (sMusicReadBuf != NULL) ? frames : 0;
	}
	if (sMusicReadBuf == NULL)
	{
		return;
	}

	int framesRead = 0;
	while (framesRead < frames)
	{
		const long bytesRemaining = sMusicDataBytes - sMusicPos;
		long framesRemaining = bytesRemaining / 4;
		if (framesRemaining <= 0)
		{
			if (sMusicLooping)
			{
				fseek(sMusicFile, sMusicDataOffset, SEEK_SET);
				sMusicPos = 0;
				continue;
			}
			else
			{
				sMusicPlaying = false;
				break;
			}
		}

		long wantFrames = frames - framesRead;
		if (wantFrames > framesRemaining)
		{
			wantFrames = framesRemaining;
		}
		const size_t got = fread(
			sMusicReadBuf + (size_t)framesRead * 2, sizeof(short) * 2,
			(size_t)wantFrames, sMusicFile);
		if (got == 0)
		{
			sMusicPlaying = false;
			break;
		}
		sMusicPos += (long)got * 4;
		framesRead += (int)got;
	}

	for (int i = 0; i < framesRead; i++)
	{
		int l = buf[i * 2] + (sMusicReadBuf[i * 2] * sMusicVolume) / 128;
		int r = buf[i * 2 + 1] + (sMusicReadBuf[i * 2 + 1] * sMusicVolume) / 128;
		if (l > 32767) l = 32767;
		if (l < -32768) l = -32768;
		if (r > 32767) r = 32767;
		if (r < -32768) r = -32768;
		buf[i * 2]     = (short)l;
		buf[i * 2 + 1] = (short)r;
	}
}

/* Per-call, LOCAL real-time pacing - not a fixed ratio, not a cumulative
 * catch-up target. Two earlier approaches both measurably failed:
 *   - Fixed ratios (2x, 4x, 5x): the true SDL dummy-driver callback
 *     deficit isn't a stable hardware constant - it only matched ~4.24x
 *     in one test and produced worse results (both "a touch slow" and,
 *     at 5x, "very slow, hear every instrument") in others. Diagnostic
 *     logging showed WHY: once a fixed ratio asks for more than the
 *     ring's real per-interval headroom, hwaudio_write() silently drops
 *     the TAIL of the buffer - and since our buffer is naively ordered
 *     (sample 0 duplicated first, sample 511 last), that tail-drop
 *     discards real trailing content every call on top of the pitch-
 *     stretching duplication artifact - two compounding problems at once.
 *   - Cumulative-since-start catch-up (an even earlier attempt): once
 *     ANY shortfall occurred, the target vs. actual gap only grew
 *     (never decays), permanently pinning every call at its max burst
 *     cap - confirmed via logging ("poofy static").
 * The fix: track only the interval since the PREVIOUS call (real
 * CLOCK_MONOTONIC delta, no persistent backlog concept at all) and
 * generate exactly that many HWAUDIO_RATE-equivalent frames this call,
 * nearest-neighbor-stretched from whatever came in. Self-corrects to
 * whatever SDL's real cadence happens to be at any given moment -
 * never assumes a ratio, never accumulates an unrecoverable gap. */
static struct timespec sLastCallTime;
static bool sHaveLastCallTime = false;

/* Guard against one pathologically long gap (e.g. a level-load stall)
 * asking for an enormous one-shot burst - same spirit as every other
 * capped-catchup pattern on this device, just bounded much more tightly
 * since there's no backlog to actually catch up on here. */
#define MAX_FRAMES_PER_CALL HWAUDIO_RATE

void Sf3500AudioPostMix(void *udata, Uint8 *stream, int len)
{
	(void)udata;
	const short *src = (const short *)stream;
	const int in_frames = len / 4;
	if (in_frames <= 0)
	{
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long long out_frames_ll;
	if (!sHaveLastCallTime)
	{
		/* No history yet - just pass the first call through at nominal
		 * rate rather than guessing a real interval. */
		out_frames_ll = in_frames;
	}
	else
	{
		const long long elapsed_ns =
			(long long)(now.tv_sec - sLastCallTime.tv_sec) * 1000000000LL
			+ (now.tv_nsec - sLastCallTime.tv_nsec);
		out_frames_ll = elapsed_ns * HWAUDIO_RATE / 1000000000LL;
	}
	sLastCallTime = now;
	sHaveLastCallTime = true;

	if (out_frames_ll < 1)
	{
		out_frames_ll = 1;
	}
	if (out_frames_ll > MAX_FRAMES_PER_CALL)
	{
		out_frames_ll = MAX_FRAMES_PER_CALL;
	}
	const int out_frames = (int)out_frames_ll;

	if (out_frames > sUpsampleBufFrames)
	{
		free(sUpsampleBuf);
		sUpsampleBuf = malloc((size_t)out_frames * 2 * sizeof(short));
		sUpsampleBufFrames = (sUpsampleBuf != NULL) ? out_frames : 0;
	}
	if (sUpsampleBuf == NULL)
	{
		return;
	}

	for (int j = 0; j < out_frames; j++)
	{
		int src_idx = j * in_frames / out_frames;
		if (src_idx >= in_frames)
		{
			src_idx = in_frames - 1;
		}
		sUpsampleBuf[j * 2]     = src[src_idx * 2];
		sUpsampleBuf[j * 2 + 1] = src[src_idx * 2 + 1];
	}

	Sf3500MusicMixInto(sUpsampleBuf, out_frames);

	hwaudio_write(sUpsampleBuf, out_frames);
}

void Sf3500PresentRenderer(SDL_Renderer *renderer)
{
	int w, h;
	SDL_GetRendererOutputSize(renderer, &w, &h);
	if (w != sFrameBufW || h != sFrameBufH || sFrameBuf == NULL)
	{
		free(sFrameBuf);
		sFrameBuf = malloc((size_t)w * (size_t)h * sizeof(unsigned short));
		sFrameBufW = w;
		sFrameBufH = h;
	}
	if (w != sNativeBufW || h != sNativeBufH || sNativeBuf == NULL)
	{
		free(sNativeBuf);
		sNativeBuf = malloc((size_t)w * (size_t)h * sizeof(Uint32));
		sNativeBufW = w;
		sNativeBufH = h;
	}
	if (sFrameBuf == NULL || sNativeBuf == NULL)
	{
		return;
	}

	SDL_RenderReadPixels(
		renderer, NULL, 0, sNativeBuf, w * (int)sizeof(Uint32));
	for (int i = 0; i < w * h; i++)
	{
		const Uint32 px = sNativeBuf[i];
		const Uint8 r = (Uint8)(px >> 16);
		const Uint8 g = (Uint8)(px >> 8);
		const Uint8 b = (Uint8)px;
		sFrameBuf[i] =
			(Uint16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
	}
	const int pitch = w * (int)sizeof(unsigned short);
	hwdisp_present(sFrameBuf, w, h, pitch);
}

void Sf3500Deinit(void)
{
	hwaudio_deinit();
	hwdisp_deinit();
	free(sFrameBuf);
	sFrameBuf = NULL;
	free(sNativeBuf);
	sNativeBuf = NULL;
	free(sUpsampleBuf);
	sUpsampleBuf = NULL;
	sUpsampleBufFrames = 0;
	free(sMusicReadBuf);
	sMusicReadBuf = NULL;
	sMusicReadBufFrames = 0;
	if (sMusicFile != NULL)
	{
		fclose(sMusicFile);
		sMusicFile = NULL;
	}
}
