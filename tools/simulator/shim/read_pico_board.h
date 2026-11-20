#pragma once
#include <stdbool.h>

/* Host shim: read_pico_board.h stands in for the Pico power/board layer.
 * The simulator only needs the display timing helpers; rails and the I2C
 * expander live behind the offline metalio_hw stubs. */

void read_pico_epd_restore_lcd(void);
bool read_pico_epd_release_drive(void);
int read_pico_ioe_int_level(void);
