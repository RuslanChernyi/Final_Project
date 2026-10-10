#include "shadow.h"

#include <ArduinoJson.h>
#include "config.h"
#include "led.h"
#include "network.h"
#include "power.h"

bool shadowRequested = false;
bool shadowReceived = false;

static bool applyShadowState(JsonObjectConst state);

void handleShadow(const char *topic, byte *payload, unsigned int length) {
  // get/rejected з кодом 404 — Shadow ще не існує (перший запуск). Створюємо його своїм reported
  if (strcmp(topic, TOPIC_SHADOW_GET_REJECTED) == 0) {
    shadowReceived = true;
    Serial.println("[Shadow] Документа ще немає - створюємо з reported");
    reportShadow();
    return;
  }

  // ArduinoJson копіює рядки в doc, тож payload далі не потрібен —
  // і publish() у reportShadow() може спокійно перезаписати буфер PubSubClient
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char *)payload, length);
  if (err) {
    Serial.print("[Shadow] Невалідний JSON: ");
    Serial.println(err.c_str());
    return;
  }

  if (strcmp(topic, TOPIC_SHADOW_GET_ACCEPTED) == 0) {
    // повний документ: {"state":{"desired":{...},"reported":{...},"delta":{...}}, ...}
    shadowReceived = true;
    Serial.println("[Shadow] Отримали документ");
    applyShadowState(doc["state"]["desired"]);
    reportShadow();   // після старту звітуємо завжди: хмара має знати, що пристрій живий і на якій версії
    return;
  }

  // delta: {"state":{"led":"on"}, ...} — лише поля, де desired != reported
  Serial.println("[Shadow] Отримали delta");
  if (applyShadowState(doc["state"])) {
    reportShadow();
  }
}

// Застосовує {"led":"on","interval_sec":60}. Повертає true, якщо щось змінилось
static bool applyShadowState(JsonObjectConst state) {
  if (state.isNull()) {
    return false;   // desired порожній — лишаємо як є
  }
  bool changed = false;

  if (state["led"].is<const char *>()) {
    const char *led = state["led"];
    if (strcmp(led, "on") == 0 || strcmp(led, "off") == 0) {
      bool on = (strcmp(led, "on") == 0);
      if (on != ledOn) {
        setLed(on);
      }
      changed = true;   // навіть без зміни LED — reported треба вирівняти з desired
    } else {
      Serial.print("[Shadow] Невідоме значення led: ");
      Serial.println(led);
    }
  }

  if (state["interval_sec"].is<uint32_t>()) {
    uint32_t sec = state["interval_sec"];
    // поза межами — ігноруємо, а не обрізаємо: обрізане значення ніколи не збіглося б
    // з desired, і delta приходила б знову і знову
    if (sec >= MIN_INTERVAL_SEC && sec <= MAX_INTERVAL_SEC) {
      publishInterval = sec * 1000;
      changed = true;
      Serial.print("[Shadow] Інтервал публікації: ");
      Serial.print(sec);
      Serial.println(" с");
    } else {
      Serial.print("[Shadow] interval_sec поза межами ");
      Serial.print(MIN_INTERVAL_SEC);
      Serial.print("-");
      Serial.print(MAX_INTERVAL_SEC);
      Serial.println(" - ігноруємо");
    }
  }
  return changed;
}

// Звітуємо, що пристрій реально робить зараз
void reportShadow(void) {
  char payload[160];
  snprintf(payload, sizeof(payload),
      "{\"state\":{\"reported\":{\"led\":\"%s\",\"interval_sec\":%lu,\"firmware_version\":\"%s\"}}}",
      ledOn ? "on" : "off", (unsigned long)(publishInterval / 1000), FIRMWARE_VERSION);

  Serial.print("[Shadow] reported: ");
  Serial.println(payload);

  bool ok = mqttClient.publish(TOPIC_SHADOW_UPDATE, payload);
  Serial.println(ok ? "[MQTT] OK" : "[MQTT] Помилка публікації");
}
