#pragma once
#include "epdiy.h"
typedef struct{uint8_t *front_fb,*back_fb; int temperature; void *driver;} EpdiyHighlevelState;
void epd_hl_set_all_white(EpdiyHighlevelState*);
uint8_t *epd_hl_get_framebuffer(EpdiyHighlevelState*);
/* S31 display pipeline surface: the simulator presents the hl framebuffer
 * directly, so every update variant just pushes the current frame. */
enum EpdDrawError epd_hl_update_screen(EpdiyHighlevelState*,enum EpdDrawMode,int);
enum EpdDrawError epd_hl_update_screen_full(EpdiyHighlevelState*,enum EpdDrawMode,int);
enum EpdDrawError epd_hl_update_screen_from_white(EpdiyHighlevelState*,enum EpdDrawMode,int);
enum EpdDrawError epd_hl_update_area(EpdiyHighlevelState*,enum EpdDrawMode,int,EpdRect);
enum EpdDrawError epd_hl_update_area_full(EpdiyHighlevelState*,enum EpdDrawMode,int,EpdRect);
enum EpdDrawError epd_hl_update_area_diff_exact(EpdiyHighlevelState*,enum EpdDrawMode,int,EpdRect);
enum EpdDrawError epd_hl_update_area_full_exact(EpdiyHighlevelState*,enum EpdDrawMode,int,EpdRect);
void epd_hl_waveform(EpdiyHighlevelState*,const EpdWaveform*);
void epd_hl_last_timing(int*,int*,int*);
void epd_fullclear(EpdiyHighlevelState*,int);
