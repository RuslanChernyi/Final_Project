#ifndef POWER_H
#define POWER_H

#include <stdint.h>

// ═══ ЩО ПЕРЕЖИВАЄ СОН: RTC_DATA_ATTR ═══
// Deep Sleep вимикає основну RAM — звичайні змінні після пробудження знову мають початкові значення.
// RTC-пам'ять (8 КБ) живе між пробудженнями, але не переживає втрату живлення і RESET.
// Визначені (з RTC_DATA_ATTR) у power.cpp
extern bool ledOn;
extern uint32_t publishInterval;   // мс, змінюється через desired.interval_sec
extern uint32_t bootCount;

uint32_t uptimeSec(void);
bool initRtcState(void);
void runWakeCycle(bool wokeByButton);
void goToSleep(void);

#endif // POWER_H
