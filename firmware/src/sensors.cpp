#include "sensors.h"

#include <Arduino.h>
#include <DHT.h>
#include "config.h"

static DHT dht(SENSOR_DHT_PIN, DHTT_TYPE);

void initSensors(void) {
  pinMode(LIGHT_SENSOR_PIN, INPUT);
  analogReadResolution(12);   // 0..4095
  dht.begin();
}

s_dhtData get_dhtData(void) {
  s_dhtData data = {0};
  // Get the sensor values
  float temp = dht.readTemperature();
  float humidity = dht.readHumidity();

  if (!isnan(temp) && !isnan(humidity)) {
    data.status = STATUS_OK;
    data.humidity = humidity;
    data.temperature = temp;
  } else {
    data.status |= STATUS_DHT_ERR;
    data.humidity = 0.0f;
    data.temperature = 0.0f;
  }
  return data;
}

// Модуль фоторезистора в Wokwi: що світліше, то нижча напруга на AO.
// Перевертаємо, щоб light_level ріс разом з освітленістю: 0 = темно, 4095 = дуже світло
uint16_t readLightLevel(void) {
  return ADC_MAX - analogRead(LIGHT_SENSOR_PIN);
}
