#pragma once
#include <stdbool.h>
typedef struct pmu_snapshot pmu_snapshot_t;
const pmu_snapshot_t *read_pico_pmu_get(void);
int pmu_battery_percent(const pmu_snapshot_t *snapshot);
bool pmu_battery_charging(const pmu_snapshot_t *snapshot);
