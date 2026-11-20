#pragma once
typedef struct{int dummy;} lcd_bus_config_t;
/* S31 display pipeline: prefill lines are a hardware detail in the simulator. */
int epd_lcd_set_prefill_lines(int n);
