#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>

// DHT22 (температура, вологість) і фоторезистор (освітленість)

typedef struct {
  float humidity;
  float temperature;
  uint8_t status;
} s_dhtData;

void initSensors(void);
s_dhtData get_dhtData(void);
uint16_t readLightLevel(void);

#endif // SENSORS_H
