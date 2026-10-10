#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

// Усе, що пристрій публікує в telemetry і events

void publishReading(void);
void publishSensorData(float temperature, float humidity, uint16_t light);
void publishSensorError(void);
void publishManualTrigger(void);
void publishLedEvent(bool ledOn);

#endif // TELEMETRY_H
