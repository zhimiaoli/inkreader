#include "host.h"
#include "read_pico_sd.h"
#include "read_pico_pmu.h"
#include "read_pico_transfer.h"
#include "read_pico_search.h"
#include "display.h"
#include "epd_highlevel.h"
#include "esp_app_desc.h"
#include "boot_state.h"
#include "esp_vfs_fat.h"
#include "sc7a20h.h"
#include "sc7a20h_lab.h"
#include "esp_timer.h"
#include "ff.h"
#include "ble_page_turner.h"
#if KIIKO_BOARD_METALIO47
#include "metalio_hw.h"
#include "metalio_charger.h"
#endif
#include "ota_online.h"
#include "ota_update.h"
#include "app.h"
#include <string.h>
#include <time.h>
#include <sys/statvfs.h>

const EpdWaveform E0470_WAVEFORM={0},E0470_FOLLOW_WAVEFORM={0},E0470_TEXTTURN_WAVEFORM={0};
static bool wifi;
void sim_wifi_toggle(void){wifi=!wifi;}
bool sim_wifi_on(void){return wifi;}
int read_pico_sd_get_info(read_pico_sd_info_t*i){memset(i,0,sizeof(*i));i->present=i->mounted=true;strcpy(i->name,"Mac");i->capacity_bytes=16ULL*1024*1024*1024;i->free_bytes=8ULL*1024*1024*1024;return 0;}
int read_pico_sd_start_probe(void){return 0;}int read_pico_sd_remount(void){return 0;}int read_pico_sd_sync(void){return 0;}
int read_pico_sd_format(void){return ESP_ERR_NOT_SUPPORTED;}
const pmu_snapshot_t *read_pico_pmu_get(void){static pmu_snapshot_t p;p.present=p.status_ok=p.identity_ok=true;p.flags=PMU_STATUS_BATTERY_VALID;p.soc_permille=780;p.time_synced=true;p.unix_sec=time(NULL);return &p;}
void read_pico_transfer_get_status(read_pico_transfer_status_t*s){memset(s,0,sizeof(*s));s->network_ready=wifi;s->mode=READ_PICO_TRANSFER_MODE_STA;}
int read_pico_transfer_get_saved_wifi(char*out,bool*configured){if(out)strcpy(out,"Mac 模拟网络");if(configured)*configured=wifi;return 0;}
void epd_hl_set_all_white(EpdiyHighlevelState*h){if(h->front_fb)memset(h->front_fb,255,1216*684/2);}
uint8_t *epd_hl_get_framebuffer(EpdiyHighlevelState*h){return h->front_fb;}
void sim_hl_present(EpdiyHighlevelState*h);
void sim_hl_clear_white(EpdiyHighlevelState*h);
enum EpdDrawError epd_hl_update_screen(EpdiyHighlevelState*h,enum EpdDrawMode m,int t){(void)m;(void)t;sim_hl_present(h);return EPD_DRAW_SUCCESS;}
enum EpdDrawError epd_hl_update_screen_full(EpdiyHighlevelState*h,enum EpdDrawMode m,int t){(void)m;(void)t;sim_hl_present(h);return EPD_DRAW_SUCCESS;}
enum EpdDrawError epd_hl_update_screen_from_white(EpdiyHighlevelState*h,enum EpdDrawMode m,int t){(void)m;(void)t;sim_hl_present(h);return EPD_DRAW_SUCCESS;}
enum EpdDrawError epd_hl_update_area(EpdiyHighlevelState*h,enum EpdDrawMode m,int t,EpdRect a){(void)m;(void)t;(void)a;sim_hl_present(h);return EPD_DRAW_SUCCESS;}
enum EpdDrawError epd_hl_update_area_full(EpdiyHighlevelState*h,enum EpdDrawMode m,int t,EpdRect a){(void)m;(void)t;(void)a;sim_hl_present(h);return EPD_DRAW_SUCCESS;}
enum EpdDrawError epd_hl_update_area_diff_exact(EpdiyHighlevelState*h,enum EpdDrawMode m,int t,EpdRect a){(void)m;(void)t;(void)a;sim_hl_present(h);return EPD_DRAW_SUCCESS;}
enum EpdDrawError epd_hl_update_area_full_exact(EpdiyHighlevelState*h,enum EpdDrawMode m,int t,EpdRect a){(void)m;(void)t;(void)a;sim_hl_present(h);return EPD_DRAW_SUCCESS;}
void epd_hl_waveform(EpdiyHighlevelState*h,const EpdWaveform*w){(void)h;(void)w;}
void epd_hl_last_timing(int*d,int*p,int*c){if(d)*d=0;if(p)*p=0;if(c)*c=0;}
void epd_fullclear(EpdiyHighlevelState*h,int t){(void)t;sim_hl_clear_white(h);}
void epd_clear(void){sim_hl_clear_white(NULL);}
enum EpdDrawError epd_clear_checked(void){sim_hl_clear_white(NULL);return EPD_DRAW_SUCCESS;}
int epd_lcd_set_prefill_lines(int n){return n;}
#if !KIIKO_BOARD_METALIO47
void guard_draw_result(EpdiyHighlevelState*h,enum EpdDrawError e){(void)h;(void)e;}
void rails_keepalive(void){}void display_set_bulk_io(bool active){(void)active;}
enum EpdDrawError update_display_mode(EpdiyHighlevelState*h,enum EpdDrawMode m){return 0;}
enum EpdDrawError update_display_mode_diff(EpdiyHighlevelState*h,enum EpdDrawMode m){return 0;}
enum EpdDrawError update_display_full(EpdiyHighlevelState*h){return 0;}
enum EpdDrawError update_display_from_white(EpdiyHighlevelState*h){return 0;}
enum EpdDrawError update_display_fast_page(EpdiyHighlevelState*h){return 0;}
enum EpdDrawError update_display_white(EpdiyHighlevelState*h){return 0;}
bool display_take_white_exit(void){return false;}
void display_main_transition_cancel(void){}
void display_main_transition_arm(const void*p,bool a,bool b){}
void display_main_transition_disarm(void){}
enum EpdDrawError update_display_area_with(EpdiyHighlevelState*h,const EpdWaveform*w,enum EpdDrawMode m,EpdRect a){return 0;}
enum EpdDrawError update_display_area_diff_with(EpdiyHighlevelState*h,const EpdWaveform*w,enum EpdDrawMode m,EpdRect a){return 0;}
enum EpdDrawError update_display_area_full_with(EpdiyHighlevelState*h,const EpdWaveform*w,enum EpdDrawMode m,EpdRect a){return 0;}
#endif
const esp_app_desc_t *esp_app_get_description(void){static esp_app_desc_t a={.version="0.3.3-rc97",.project_name="Pico",.date="2026-10-12",.idf_ver="Mac simulator"};return &a;}
bool pico_boot_asset_allowed(const char*p){return true;}
bool pico_boot_recovery(void){return false;}
void pico_boot_book_end(bool ok){}
void pico_boot_clear_resume(void){}
void pico_boot_hold_resume(void){}
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
EM_JS(void,sim_random,(void *p,size_t n),{crypto.getRandomValues(HEAPU8.subarray(p,p+n));});
void esp_fill_random(void*p,size_t n){sim_random(p,n);}
#else
void esp_fill_random(void*p,size_t n){arc4random_buf(p,n);}
#endif
int esp_vfs_fat_spiflash_mount_rw_wl(const char*p,const char*label,const esp_vfs_fat_mount_config_t*c,wl_handle_t*w){*w=1;sim_mkdir(p,0700);return 0;}
int esp_vfs_fat_info(const char*p,uint64_t*total,uint64_t*free){*total=16ULL*1024*1024*1024;*free=8ULL*1024*1024*1024;return 0;}
int f_getfree(const char*p,DWORD*n,FATFS**fs){static FATFS f={.csize=8,.n_fatent=2000000};*fs=&f;*n=1000000;return 0;}

const EpdWaveform E0470_FULL_WAVEFORM={0};
const EpdWaveform *display_reader_text_waveform(void){return &E0470_TEXTTURN_WAVEFORM;}
#if !KIIKO_BOARD_METALIO47
void display_main_transition_shelf_page(void){}
enum EpdDrawError update_display_with(EpdiyHighlevelState*h,const EpdWaveform*w,enum EpdDrawMode m){return 0;}
enum EpdDrawError update_display_water_turn(EpdiyHighlevelState*h,EpdRect a,e0470_turn_dir_t d){return 0;}
#endif
static int turn_tick=14000;
void e0470_page_turn_set_tick_us(int n){turn_tick=n;}int e0470_page_turn_tick_us(void){return turn_tick;}void e0470_page_turn_release(void){}
bool app_poll_main_navigation(app_ctx_t*c){return false;}
int ui_key_hit_test(uint16_t x,uint16_t y){return y>=1216?(x<228?0:x<456?1:2):-1;}
int pico_boot_book_begin(const char*p){return 0;}
void pmu_selftest_prepare_powerdown(void){}
int read_pico_pmu_cmd(uint16_t c,const uint8_t*p,uint8_t n){return ESP_ERR_NOT_SUPPORTED;}
void esp_restart(void){}
int read_pico_transfer_sync_time(uint32_t*s){return ESP_ERR_NOT_SUPPORTED;}
int read_pico_transfer_import_wifi_backup(const read_pico_transfer_wifi_backup_t*b){return ESP_ERR_NOT_SUPPORTED;}
int read_pico_transfer_export_wifi_backup(read_pico_transfer_wifi_backup_t*b){memset(b,0,sizeof(*b));return ESP_ERR_NOT_SUPPORTED;}
bool read_pico_transfer_wifi_backup_valid(const read_pico_transfer_wifi_backup_t*b){return false;}
static bool checked;
int pico_online_check(void){checked=true;return ESP_ERR_NOT_SUPPORTED;}int pico_online_download(void){return ESP_ERR_NOT_SUPPORTED;}int pico_online_commit(void){return ESP_ERR_NOT_SUPPORTED;}
bool pico_online_busy(void){return false;}void pico_online_cancel_join(void){}
void pico_online_get_status(pico_update_status_t*s){memset(s,0,sizeof(*s));s->state=checked?PICO_UPDATE_FAILED:PICO_UPDATE_IDLE;snprintf(s->message,sizeof(s->message),"模拟器尚未接入联网升级");}
int pico_ota_inspect(const char*p,pico_ota_info_t*i){memset(i,0,sizeof(*i));snprintf(i->current_version,sizeof(i->current_version),"0.3.3-rc97");snprintf(i->message,sizeof(i->message),"模拟器暂不运行设备BIN");return ESP_ERR_NOT_SUPPORTED;}
int pico_ota_install(const char*p,char*m,size_t n){snprintf(m,n,"请在真实Pico设备上升级");return ESP_ERR_NOT_SUPPORTED;}
void ble_pt_reset_failure(void){}int ble_pt_stop(uint32_t n){return 0;}bool ble_pt_running(void){return false;}
bool ble_pt_take_start_failure(char*out,size_t n){return false;}
void ble_pt_scan_start(uint32_t n){}void ble_pt_scan_stop(void){}bool ble_pt_scanning(void){return false;}uint8_t ble_pt_device_count(void){return 0;}const ble_pt_device_t *ble_pt_device(uint8_t n){return NULL;}
int ble_pt_connect(const char*p){return ESP_ERR_NOT_SUPPORTED;}void ble_pt_disconnect(void){}bool ble_pt_connected(void){return false;}bool ble_pt_connecting(void){return false;}const char *ble_pt_connected_name(void){return "";}
uint8_t ble_pt_bond_count(void){return 0;}const ble_pt_bond_t*ble_pt_bond(uint8_t n){return NULL;}
#if KIIKO_BOARD_METALIO47
esp_err_t ble_pt_forget(const char*p){return ESP_OK;}
#else
void ble_pt_forget(const char*p){}
#endif
#if KIIKO_BOARD_METALIO47
/* Offline metalio_hw: no expander, rails, haptics or frontlight PWM. */
esp_err_t metalio_hw_init(void){return ESP_OK;}
esp_err_t metalio_tps_prepare(void){return ESP_OK;}
void metalio_tps_cancel_prepare(void){}
bool metalio_key_down(void){return false;}
bool metalio_confirm_power_key(void){return false;}
bool metalio_confirm_light_wake_key(void){return false;}
bool metalio_boot_power_key(void){return false;}
void metalio_capture_boot_power_key(void){}
int metalio_take_volume(void){return 0;}
int metalio_take_ai(void){return METALIO_AI_NONE;}
void metalio_ai_clear(void){}
void metalio_volume_clear(void){}
void metalio_ai_sample(bool down,int64_t now_ms){(void)down;(void)now_ms;}
void metalio_volume_sample(uint16_t inputs,int64_t now_ms){(void)inputs;(void)now_ms;}
bool metalio_frontlight_enabled(void){return false;}
unsigned metalio_frontlight_position(bool warmth){(void)warmth;return 0;}
void metalio_frontlight_preview_position(unsigned brightness,unsigned warmth){(void)brightness;(void)warmth;}
void metalio_frontlight_preview(unsigned brightness,unsigned warmth){(void)brightness;(void)warmth;}
void metalio_frontlight_toggle(void){}
void metalio_frontlight_commit(void){}
void metalio_frontlight_save(unsigned brightness,unsigned warmth){(void)brightness;(void)warmth;}
void metalio_frontlight_restore(void){}
void metalio_frontlight(unsigned brightness,unsigned warmth){(void)brightness;(void)warmth;}
unsigned metalio_brightness(void){return 0;}
unsigned metalio_warmth(void){return 0;}
bool metalio_haptic_enabled(void){return false;}
void metalio_haptic_set_enabled(bool enabled){(void)enabled;}
esp_err_t metalio_haptic_click(void){return ESP_ERR_NOT_SUPPORTED;}
void metalio_haptic_stop(void){}
void metalio_haptic_resume(void){}
bool metalio_download_connected(void){return false;}
void metalio_clear_download_request(void){}
bool metalio_usb_blocks_sleep(void){return false;}
void metalio_usb_describe(char *out,size_t size){if(out&&size)snprintf(out,size,"sim");}
void metalio_poweroff(void){}
void metalio_hold_for_deep_sleep(void){}
void metalio_sd_release_hold(void){}
esp_err_t metalio_charger_init(void){return ESP_OK;}
bool metalio_charger_ready(void){return false;}
esp_err_t metalio_charger_set_current_ma(uint32_t ma){(void)ma;return ESP_ERR_INVALID_STATE;}
uint32_t metalio_charger_current_ma(void){return 0;}
esp_err_t metalio_charger_set_fast_charge(bool on){(void)on;return ESP_ERR_INVALID_STATE;}
bool metalio_charger_fast_charge(void){return false;}
void metalio_charger_suspend(void){}
void metalio_charger_resume(void){}
#endif
bool ble_pt_pop_key(ble_pt_event_t*e){return false;}bool ble_pt_pop_raw(ble_pt_raw_t*r){return false;}
uint32_t ble_pt_raw_code(const ble_pt_raw_t*r){return 0;}
int ble_pt_bind(ble_pt_action_t a,const ble_pt_frame_t *frame){return ESP_ERR_NOT_SUPPORTED;}bool ble_pt_binding(ble_pt_action_t a,ble_pt_frame_t *out){return false;}
ble_pt_action_t ble_pt_action_for_raw(uint32_t code){return BLE_PT_ACTION_NONE;}
ble_pt_action_t ble_pt_action_for_usage(uint8_t usage,uint8_t mods){return BLE_PT_ACTION_NONE;}
#include "ui_nav.h"
#include "ui_kit.h"
static void transfer_render(app_ctx_t*c,uint8_t*f){ui_clear_page(f);ui_nav_status(f);ui_draw_header(f,"文件传输","Mac 模拟器");ui_text(f,342,430,30,"通过模拟器侧栏导入书籍和字体",EPD_DRAW_ALIGN_CENTER,false);ui_text(f,342,510,24,"WiFi / USB 硬件传输尚未接入",EPD_DRAW_ALIGN_CENTER,false);ui_nav_draw(f,2);}
static app_redraw_t transfer_key(app_ctx_t*c,int k){c->request_return=true;return APP_REDRAW_PAGE;}
const app_desc_t app_transfer={.title="文件传输",.render=transfer_render,.on_key=transfer_key};
void app_transfer_request_hotspot_start(void){}void app_transfer_request_usb_start(void){}void app_transfer_request_wifi_setup(void){}void app_transfer_request_wifi_upload(void){}
#if !KIIKO_BOARD_METALIO47
enum EpdDrawError update_display_image_gray(EpdiyHighlevelState*h){return 0;}
#endif

void ble_pt_learn_begin(void){}
ble_pt_action_t ble_pt_action_for_frame(const ble_pt_frame_t*f){return BLE_PT_ACTION_NONE;}
#if !KIIKO_BOARD_METALIO47
void display_main_transition_gray_bands(const EpdRect*b,unsigned count){}
enum EpdDrawError display_panel_repair(EpdiyHighlevelState*h){return 0;}
#endif
int read_pico_sd_refresh_space(void){return 0;}
void rp_boot_mark(const char*n){}
const EpdWaveform *display_main_bw_waveform(void){return &E0470_WAVEFORM;}
const EpdWaveform *display_main_gray_waveform(void){return &E0470_WAVEFORM;}
const EpdWaveform *display_main_normal_waveform(void){return &E0470_WAVEFORM;}
const EpdWaveform *display_main_fast_white_waveform(const EpdWaveform*source,enum EpdDrawMode mode){(void)source;(void)mode;return &E0470_WAVEFORM;}
const EpdWaveform *display_settings_motion_waveform(void){return &E0470_WAVEFORM;}
const EpdWaveform *display_settings_scroll_waveform(void){return &E0470_WAVEFORM;}
const EpdWaveform *display_reader_soft_turn_waveform(void){return &E0470_WAVEFORM;}
enum EpdDrawError update_display_reader_area(EpdiyHighlevelState*h,const EpdWaveform*w,enum EpdDrawMode m,EpdRect a,bool f){(void)h;(void)w;(void)m;(void)a;(void)f;return 0;}
#if !KIIKO_BOARD_METALIO47
enum EpdDrawError update_display_text_turn(EpdiyHighlevelState*h,EpdRect a,e0470_turn_dir_t d){(void)h;(void)a;(void)d;return 0;}
#endif
void continuous_du_reset(void){}
bool app_touch_input_pending(app_ctx_t*c){(void)c;return false;}
void app_restart_host(void){esp_restart();}
void app_transfer_request_cover_wifi(void){}
bool read_pico_sd_io_ready(void){return true;}
esp_err_t read_pico_sd_start_space_refresh(void){return 0;}
esp_err_t read_pico_transfer_start(const read_pico_transfer_cfg_t*c){(void)c;return ESP_ERR_NOT_SUPPORTED;}
void read_pico_transfer_stop(void){}
bool read_pico_transfer_try_stop_if_idle(void){return true;}
void ble_pt_cancel_recent(void){}
void ble_pt_reconnect_recent(void){}
