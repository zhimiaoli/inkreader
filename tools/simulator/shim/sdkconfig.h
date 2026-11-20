#pragma once

/* Host shim: firmware sdkconfig. TJpgDec reads its JD_* options from here;
 * anything undefined falls back to the component defaults, and leaving
 * CONFIG_JD_USE_ROM unset keeps the ESP32-only ROM path out of host builds. */
