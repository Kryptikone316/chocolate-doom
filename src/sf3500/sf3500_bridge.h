/* SF3500 device bridge for chocolate-doom: wires the existing SDL2
 * video/audio paths to the real hardware wrappers (hwdisp/hwaudio), and
 * drives Doom's own keyboard event queue directly from joykey_read()
 * instead of going through SDL's joystick/GameController layer (confirmed
 * broken for virtual joysticks on this device's SDL2 build - see
 * cdogs_sdl_handoff.md). Doom's default keyboard bindings (arrows for
 * movement, RCTRL/space/RALT/RSHIFT for fire/use/strafe/speed, TAB/ESC for
 * map/menu) already map cleanly onto a d-pad + 6 buttons with zero config
 * changes needed. */
#ifndef SF3500_BRIDGE_H
#define SF3500_BRIDGE_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, right after SDL_Init() succeeds. Inits hwdisp/hwaudio/joykey. */
void Sf3500Init(void);

/* Call once per tic (I_StartTic() in i_video.c, alongside I_GetEvent()).
 * Posts synthetic ev_keydown/ev_keyup events for the game's own default
 * keyboard bindings, driven by real joykey_read() transitions, and exits
 * directly (bypassing the game's own menu/quit path) if SELECT+START are
 * held together. */
void Sf3500PumpInput(void);

/* SDL_mixer Mix_SetPostMix callback: forwards the fully-mixed PCM straight
 * into hwaudio_write() - format already matches HWAUDIO_RATE exactly, no
 * resampling needed. */
void Sf3500AudioPostMix(void *udata, Uint8 *stream, int len);

/* Bridges one composited frame from an SDL_Renderer to hwdisp_present():
 * reads it back in the renderer's native format (no SDL-side conversion,
 * which measured ~100x slower on real hardware - see cdogs_sdl_handoff.md),
 * does the RGB565 downconvert ourselves, and hands it to the real display
 * driver. Call this INSTEAD OF SDL_RenderPresent(). */
void Sf3500PresentRenderer(SDL_Renderer *renderer);

/* Call once at shutdown, before SDL_Quit(). */
void Sf3500Deinit(void);

/* Substitute-music playback, entirely bypassing SDL_mixer's Mix_Chunk/
 * Mix_Music machinery. Root cause, confirmed via real per-call timing
 * instrumentation on real hardware (2026-10-10): SDL's own audio-
 * generation callback on this device only fires at a fraction of the
 * real-time rate its opened spec implies (measured ~11225Hz effective
 * vs the 24000Hz asked for, a stable ~4.27x deficit) - and since SDL/
 * SDL_mixer's playback-position tracking is driven ENTIRELY by callback
 * invocation count (not a real-time clock), the music itself advances
 * through the song at that same fraction of real speed. No amount of
 * resampling on the OUTPUT side can fix this, since we're only ever
 * reshaping content SDL already generated too slowly - confirmed
 * separately that hwaudio's own delivery-to-hardware pacing is correct
 * (consumer-thread diagnostic: ~10ms wake interval, ~38us play() calls,
 * healthy ring). This streams the WAV file directly from disk (not
 * loaded fully into RAM - same OOM risk Mix_Chunk had for these large
 * tracks), paced by our own real CLOCK_MONOTONIC reads, and mixes the
 * result into hwaudio's output from Sf3500AudioPostMix - fully
 * independent of SDL's broken callback cadence. */
int Sf3500MusicLoad(const char *path);
void Sf3500MusicPlay(int loop);
void Sf3500MusicStop(void);
void Sf3500MusicSetVolume(int volume_0_to_128);
int Sf3500MusicIsPlaying(void);

#ifdef __cplusplus
}
#endif

#endif
