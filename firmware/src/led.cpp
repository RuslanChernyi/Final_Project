#include "led.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "power.h"
#include "shadow.h"
#include "telemetry.h"

// Єдине місце, де змінюється LED: і для commands/led, і для Shadow
void setLed(bool on) {
  ledOn = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  Serial.println(on ? "[LED] Увімкнено" : "[LED] Вимкнено");

  // confirm only after the command was actually executed
  publishLedEvent(on);
}

void handleLedCommand(const char *message, unsigned int length) {
  // expected: {"action":"set_led","value":"on"} or {"action":"set_led","value":"off"}
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, message, length);
  if (err) {
    Serial.print("[LED] Невалідний JSON: ");
    Serial.println(err.c_str());
    return;
  }

  const char *action = doc["action"] | "";  // "" if the field is missing
  const char *value  = doc["value"]  | "";

  if (strcmp(action, "set_led") != 0) {
    Serial.print("[LED] Невідома дія: ");
    Serial.println(action);
    return;
  }

  bool turnOn;
  if (strcmp(value, "on") == 0) {
    turnOn = true;
  } else if (strcmp(value, "off") == 0) {
    turnOn = false;
  } else {
    Serial.print("[LED] Невідоме значення: ");
    Serial.println(value);
    return;
  }

  setLed(turnOn);

  // Команда міняє лише reported. Якщо в Shadow є desired.led з іншим значенням,
  // AWS одразу надішле delta, і LED повернеться до desired — Shadow має пріоритет
  reportShadow();
}

void bootBlink(void) {
  for (int i = 0; i < BOOT_BLINKS; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
}
