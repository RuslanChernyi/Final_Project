#include "network.h"

#include <WiFi.h>
#include <esp_ota_ops.h>
#include "config.h"
#include "secrets.h"   // лише тут: сертифікати визначені в заголовку, друге підключення = дублікат при лінкуванні
#include "led.h"
#include "ota.h"
#include "shadow.h"

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

static void onMessage(char *topic, byte *payload, unsigned int length);

bool connectWifi() {
  Serial.println("Connecting to Wifi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT) {
      Serial.println("timeout");
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println(" OK");
  Serial.println("WiFi IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool isWifiConnected() { return WiFi.status() == WL_CONNECTED; }

void configureTLS(void){
 wifiClient.setCACert(root_ca);
 wifiClient.setCertificate(device_cert);
 wifiClient.setPrivateKey(private_key);
}

bool syncNtpTime(void){
  configTime(0, 0, "pool.ntp.org");
  if (!isWifiConnected()) {
    return false;
  }

  Serial.print("Syncing time");
  uint32_t start = millis();
  while (time(nullptr) < NTP_VALID_EPOCH) {
    if (millis() - start > NTP_TIMEOUT) {
      Serial.println(" timeout");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Time synced");
  return true;
}

time_t currentTimestamp(void) {
  time_t now = time(nullptr);
  return (now < NTP_VALID_EPOCH) ? 0 : now;  // 0 = NTP not synced yet
}

bool connectMqtt() {

  // callback function for subscribed topics
  mqttClient.setCallback(onMessage);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(30);
  
  Serial.print("[MQTT] Підключаємось до ");
  Serial.print(DOMAIN_ADDRESS);
  Serial.print("...");

  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println("OK");
    mqttClient.subscribe(TOPIC_COMMANDS, 1);
    Serial.print("[MQTT] Підписатись на: ");
    Serial.println(TOPIC_COMMANDS);

    // Jobs: notify-next — якщо роботу створять, поки ми онлайн
    mqttClient.subscribe(TOPIC_JOBS_NOTIFY);
    mqttClient.subscribe(TOPIC_JOBS_GET_ACCEPTED);
    Serial.println("[MQTT] Підписатись на: jobs/notify-next, jobs/$next/get/accepted");

    // Shadow: відповідь на наш get і зміни desired
    mqttClient.subscribe(TOPIC_SHADOW_GET_ACCEPTED);
    mqttClient.subscribe(TOPIC_SHADOW_GET_REJECTED);
    mqttClient.subscribe(TOPIC_SHADOW_DELTA);
    Serial.println("[MQTT] Підписатись на: shadow/get/accepted, shadow/get/rejected, shadow/update/delta");

    // Нова прошивка довела, що працює: Wi-Fi, TLS і MQTT піднялись.
    // Тільки тепер скасовуємо rollback. Без цього перший же reset поверне стару версію
    if (!appConfirmed) {
      esp_ota_mark_app_valid_cancel_rollback();
      appConfirmed = true;
      Serial.println("[OTA] Прошивку підтверджено (rollback скасовано)");
    }

    jobsRequested = false;   // після кожного підключення — запитати роботи (див. loop())
    shadowRequested = false; // і документ Shadow
    return true;
  }

  Serial.print("Помилка: ");
  Serial.println(mqttClient.state());
  return false;
}

static void onMessage(char *topic, byte *payload, unsigned int length){
  // ═══ КОЛБЕК — ДИСПЕТЧЕР: у PubSubClient він один на всі топіки ═══

  // Є нова робота — просимо документ (у ньому буде свіжий presigned URL)
  if (strcmp(topic, TOPIC_JOBS_NOTIFY) == 0) {
    Serial.println("[OTA] notify-next - просимо документ роботи");
    mqttClient.publish(TOPIC_JOBS_GET, "{}");
    return;
  }

  // Документ приїхав. Копіюємо у свій буфер і виходимо — роботу зробить loop()
  if (strcmp(topic, TOPIC_JOBS_GET_ACCEPTED) == 0) {
    if (length >= sizeof(jobBuf)) {
      Serial.println("[OTA] Документ роботи завеликий для буфера");
      return;
    }
    memcpy(jobBuf, payload, length);
    jobBuf[length] = '\0';
    jobPending = true;
    return;
  }

  // Shadow — до копіювання на стек: документ може бути на кілька КБ
  if (strcmp(topic, TOPIC_SHADOW_GET_ACCEPTED) == 0 ||
      strcmp(topic, TOPIC_SHADOW_GET_REJECTED) == 0 ||
      strcmp(topic, TOPIC_SHADOW_DELTA) == 0) {
    handleShadow(topic, payload, length);
    return;
  }

  // payload is NOT null-terminated - copy it into a proper C string for printing
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';

  Serial.print("[MQTT] Топік: ");
  Serial.print(topic);
  Serial.print(" | Payload: ");
  Serial.println(message);

  // exact match, not a prefix - ignore anything that isn't the LED command topic
  if (strcmp(topic, TOPIC_COMMANDS) != 0) {
    return;
  }

  handleLedCommand(message, length);
}

// attempts = скільки спроб підряд уже не вдалося: 0 → 1 с, 1 → 2 с, 2 → 4 с ... максимум 30 с
uint32_t reconnectDelay(uint32_t attempts) {
  if (attempts > 5) {
    attempts = 5;   // 1000 << 5 = 32 с — далі зсувати немає сенсу, все одно обріжемо до 30
  }
  uint32_t delayMs = RECONNECT_BASE_DELAY << attempts;
  return (delayMs > RECONNECT_MAX_DELAY) ? RECONNECT_MAX_DELAY : delayMs;
}

// ═══ PUBLISH НЕ СЛУХАЄ ═══
// Вхідні повідомлення приходять тільки поки крутиться mqttClient.loop() — не «коли минув час»
// і не «коли ми щось опублікували». delay() тут не допоможе: колбек мовчатиме.
// Виходимо раніше, якщо відповідь уже прийшла (*doneFlag) — кожна зайва секунда радіо = батарея
void mqttWait(uint32_t timeoutMs, volatile bool *doneFlag) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    mqttClient.loop();
    if (doneFlag != nullptr && *doneFlag) {
      return;
    }
    delay(10);
  }
}
