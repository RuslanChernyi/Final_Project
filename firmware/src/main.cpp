#include <Arduino.h>
#include <driver/rtc_io.h>
#include "config.h"
#include "button.h"
#include "led.h"
#include "network.h"
#include "ota.h"
#include "power.h"
#include "sensors.h"
#include "shadow.h"
#include "telemetry.h"

// Модулі прошивки:
//   config.h       — версія, мережа, топіки, інтервали, піни
//   network        — Wi-Fi, TLS, NTP, MQTT-підключення і колбек-диспетчер
//   sensors        — DHT22 і фоторезистор
//   telemetry      — публікація телеметрії та подій
//   led, button    — світлодіод (команди) і кнопка з debounce
//   shadow         — Device Shadow (desired/reported)
//   ota            — OTA через AWS IoT Jobs
//   power          — RTC-пам'ять і цикл Deep Sleep
// Тут — лише setup() і loop(): що і в якому порядку робить пристрій

/***    Global variables    ***/
uint32_t lastReconectionAttempt = 0;
uint32_t reconectionAttempts = 0;
uint32_t lastPublish = 0;
bool firstPublishDone = false;   // false = publish right after (re)connecting

void setup() {
  Serial.begin(115200);
  bool freshBoot = initRtcState();
  bootCount++;
  Serial.print("Старт, прошивка ");
  Serial.print(FIRMWARE_VERSION);
  Serial.print(", пробудження №");
  Serial.println(bootCount);

#if ENABLE_DEEP_SLEEP
  // пін кнопки після сну ще в RTC-режимі (див. goToSleep) — повертаємо звичайний GPIO
  rtc_gpio_deinit((gpio_num_t)SWITCH_PIN);
  // LED тримався під час сну через gpio_hold — відпускаємо, далі керуємо самі
  gpio_hold_dis((gpio_num_t)LED_PIN);
  gpio_deep_sleep_hold_dis();
#endif

  pinMode(SWITCH_PIN,INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  if (freshBoot) {
    bootBlink();              // блимаємо лише при першому старті прошивки, а не щохвилини після сну
  }
  digitalWrite(LED_PIN, ledOn ? HIGH : LOW);   // стан LED пережив сон у RTC
  initSensors();
  if (!connectWifi()) {
    // no blocking retry here - loop() keeps reconnecting with growing delays
    Serial.println("[WiFi] Не вдалося підключитись - повторимо в loop()");
  }
  configureTLS();
  if (!syncNtpTime()) {
    // SNTP keeps syncing in the background. Until then TLS rejects the AWS
    // certificate (state -2) and payloads carry timestamp 0
    Serial.println("[NTP] Час не синхронізовано - продовжуємо, timestamp буде 0");
  }
  mqttClient.setServer(DOMAIN_ADDRESS, MQTT_PORT);
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);   // до connect()

#if ENABLE_DEEP_SLEEP
  // setup() стає всією програмою: один прохід — і спати
  runWakeCycle(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0);
#else
  if (isWifiConnected()) {
    connectMqtt();
  }
#endif
}

#if ENABLE_DEEP_SLEEP
void loop() {
  // порожньо — вся робота вмістилась в один прохід setup().
  // Сюди потрапляємо, лише якщо setup() не заснув (otaInProgress) — і просто чекаємо
}
#else
void loop() {
  // ═══ ПІД ЧАС ОНОВЛЕННЯ НІЧОГО ІНШОГО НЕ РОБИМО ═══
  if (otaInProgress) return;

  uint32_t now = millis();
  static bool wasConnected = false;

  if (!mqttClient.connected()) {
    if (wasConnected) {
      // -3 = broker closed the socket (AWS does this on a Policy violation)
      wasConnected = false;
      firstPublishDone = false;
      Serial.print("[MQTT] З'єднання розірвано, state=");
      Serial.println(mqttClient.state());
    }
    // reconnect without delay(), forever: кожна невдала спроба подвоює паузу до наступної
    if ((now - lastReconectionAttempt) > reconnectDelay(reconectionAttempts)) {
      lastReconectionAttempt = now;
      reconectionAttempts++;
      if (!isWifiConnected()) {
        Serial.println("[WiFi] З'єднання втрачено - перепідключаємось...");
        WiFi.reconnect();
      } else {
        Serial.print("[MQTT] З'єднання втрачено - перепідключаємось (спроба ");
        Serial.print(reconectionAttempts);
        Serial.println(")...");
        connectMqtt();
      }
      if (!mqttClient.connected()) {
        Serial.print("[Retry] Наступна спроба через ");
        Serial.print(reconnectDelay(reconectionAttempts) / 1000);
        Serial.println(" с");
      }
    }
    return;
  }

  wasConnected = true;
  mqttClient.loop();
  reconectionAttempts = 0;   // підключились — наступний збій знову почне з 1 с

  // Shadow: одна лише підписка на delta не покаже, що змінилось, поки ми були офлайн
  // (delta приходить лише в момент зміни). Тому після кожного підключення — явний get
  if (!shadowRequested) {
    shadowRequested = true;
    mqttClient.publish(TOPIC_SHADOW_GET, "{}");
  }

  // роботу могли створити, поки ми були офлайн — ніхто нам про неї не скаже, питаємо самі.
  // Після OTA цей самий запит поверне нашу роботу ще раз — і ми звітуємо SUCCEEDED
  if (!jobsRequested) {
    jobsRequested = true;
    mqttClient.publish(TOPIC_JOBS_GET, "{}");
  }

  // документ роботи прийшов у колбек — виконуємо тут, а не в колбеку:
  // завантаження триває десятки секунд, а колбек має швидко повернутись
  if (jobPending) {
    jobPending = false;
    otaInProgress = true;
    processJob();             // успіх = перезавантаження, сюди вже не повернемось
    otaInProgress = false;    // сюди потрапляємо лише якщо не вдалося
    return;
  }

  // first reading right after connecting, then every publishInterval (from Shadow)
  if (!firstPublishDone || (now - lastPublish) > publishInterval) {
    firstPublishDone = true;
    lastPublish = now;
    publishReading();
  }

  // button not checked during disconnect - no broker to publish to
  checkButton(SWITCH_PIN);
  static uint8_t manual_published = 0;

  if (g_buttonState == BUTTON_LOGIC_HIGH) {
    // one extra reading per button press, without waiting for the timer,
    // і подія manual_trigger в events (для таблиці натискань у Grafana)
    if (manual_published == 0) {
      publishReading();
      publishManualTrigger();
      manual_published++;
    }
  } else {
    manual_published = 0;
  }
}
#endif
