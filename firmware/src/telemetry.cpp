#include "telemetry.h"

#include <Arduino.h>
#include "config.h"
#include "network.h"
#include "power.h"
#include "sensors.h"

void publishLedEvent(bool ledOn) {
  char payload[128];
  snprintf(payload, sizeof(payload),
      "{\"device_id\":\"%s\",\"timestamp\":%lu,"
      "\"event\":\"led_changed\",\"value\":\"%s\"}",
      DEVICE_PAYLOAD_ID, (unsigned long)currentTimestamp(), ledOn ? "on" : "off");

  Serial.print("[MQTT] Подія: ");
  Serial.println(payload);

  bool ok = mqttClient.publish(TOPIC_EVENTS, payload);
  Serial.println(ok ? "[MQTT] OK" : "[MQTT] Помилка публікації");
}

void publishReading(void) {
  s_dhtData dht_data = get_dhtData();

  if (dht_data.status & STATUS_DHT_ERR) {
    Serial.println("[DHT] Помилка зчитування - телеметрію пропущено");
    publishSensorError();
    return;
  }
  publishSensorData(dht_data.temperature, dht_data.humidity, readLightLevel());
}

void publishSensorData(float temperature, float humidity, uint16_t light) {
    if (!mqttClient.connected()) {
        Serial.println("[MQTT] Не підключено — пропускаємо");
        return;
    }

    time_t now = currentTimestamp();

    // snprintf обріже рядок, якщо буфер замалий — JSON стане битим,
    // і правило його не розбере. 320 байт вистачає з запасом
    // firmware_version — у хмарі видно, хто на якій версії; після OTA це доказ, що оновлення відбулось
    char payload[320];
    snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"timestamp\":%lu,"
        "\"temperature\":%.1f,\"humidity\":%.1f,\"light_level\":%u,"
        "\"firmware_version\":\"%s\",\"uptime_sec\":%lu,\"free_heap\":%lu}",
        DEVICE_PAYLOAD_ID, (unsigned long)now, temperature, humidity, light,
        FIRMWARE_VERSION, (unsigned long)uptimeSec(), (unsigned long)ESP.getFreeHeap());

    Serial.print("[MQTT] Публікуємо: ");
    Serial.println(payload);

    bool ok = mqttClient.publish(TOPIC_TELEMETRY, payload);
    Serial.println(ok ? "[MQTT] OK" : "[MQTT] Помилка публікації");
}

void publishSensorError(void) {
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] не підключено - пропускаємо");
    return;
  }

  // separate topic, so the error never lands in iot_telemetry as a fake 0.0 reading
  char payload[128];
  snprintf(payload, sizeof(payload),
      "{\"device_id\":\"%s\",\"timestamp\":%lu,\"error\":\"dht_read_failed\"}",
      DEVICE_PAYLOAD_ID, (unsigned long)currentTimestamp());

  Serial.print("[MQTT] Подія помилки: ");
  Serial.println(payload);

  bool publish_status = mqttClient.publish(TOPIC_EVENTS, payload);
  Serial.println(publish_status ? "[MQTT] OK" : "[MQTT] Помилка публікації");
}

void publishManualTrigger(void) {
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] не підключено - пропускаємо");
    return;
  }

  char payload[160];
  snprintf(payload, sizeof(payload),
      "{\"device_id\":\"%s\",\"timestamp\":%lu,"
      "\"event\":\"manual_trigger\",\"firmware_version\":\"%s\"}",
      DEVICE_PAYLOAD_ID, (unsigned long)currentTimestamp(), FIRMWARE_VERSION);

  Serial.print("[MQTT] Подія: ");
  Serial.println(payload);

  bool ok = mqttClient.publish(TOPIC_EVENTS, payload);
  Serial.println(ok ? "[MQTT] OK" : "[MQTT] Помилка публікації");
}
