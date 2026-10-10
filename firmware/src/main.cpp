#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <driver/rtc_io.h>
#include "secrets.h"

/***    Defines   ***/

// ═══ ВЕРСІЯ ЦІЄЇ ПРОШИВКИ ═══
// Зібрали нову прошивку — підняли число. Забули — OTA-job ніколи не завершиться.
#define FIRMWARE_VERSION "1.0.0"
// Видима зміна між версіями: скільки разів LED блимає при старті.
// 1.0.0 -> 1 раз, 1.1.0 -> 3 рази
#define BOOT_BLINKS 1

// Deep Sleep — додатковий режим. Вмикається окремою збіркою (platformio.ini, env:esp32dev_deepsleep):
//   0 — пристрій завжди онлайн (базовий проєкт, стаціонарне живлення)
//   1 — прокинувся → підключився → OTA? → телеметрія → Shadow → заснув на interval_sec
#ifndef ENABLE_DEEP_SLEEP
#define ENABLE_DEEP_SLEEP 0
#endif

#define BUTTON_LOGIC_HIGH 0
#define BUTTON_LOGIC_LOW 1

#define STATUS_OK 0b00000000
#define STATUS_DHT_ERR 0b00000010  // біт 1: DHT22 помилка

// WIFI settings
#define WIFI_SSID     "Wokwi-GUEST"  // мережа Wokwi симулятора
#define WIFI_PASSWORD ""              // без пароля
#define WIFI_TIMEOUT  10000           // максимум 10 секунд на підключення

// MQTT settings
#define DOMAIN_ADDRESS "a1u0a7gycxitea-ats.iot.eu-central-1.amazonaws.com"
#define MQTT_PORT 8883
#define MQTT_CLIENT_ID "esp32-Ruslan_Chernyi"
// WORKING TOPICS
#define TOPIC_COMMANDS "iot-course/Ruslan_Chernyi/commands/led"
#define TOPIC_TELEMETRY "iot-course/Ruslan_Chernyi/telemetry"
#define TOPIC_EVENTS    "iot-course/Ruslan_Chernyi/events"

#define DEVICE_PAYLOAD_ID "esp32-Ruslan_Chernyi"

// AWS IoT Jobs — зарезервовані топіки $aws, їх визначає AWS
#define THING_NAME "esp32-Ruslan_Chernyi"
#define TOPIC_JOBS_NOTIFY       "$aws/things/" THING_NAME "/jobs/notify-next"     // <- сповіщення про нову роботу
#define TOPIC_JOBS_GET          "$aws/things/" THING_NAME "/jobs/$next/get"       // -> просимо документ
#define TOPIC_JOBS_GET_ACCEPTED "$aws/things/" THING_NAME "/jobs/$next/get/accepted" // <- приходить документ

// Device Shadow — теж зарезервовані топіки $aws. Конфігурація пристрою живе в хмарі:
// desired = що хоче користувач, reported = що пристрій реально зробив, delta = різниця між ними
#define TOPIC_SHADOW_GET          "$aws/things/" THING_NAME "/shadow/get"           // -> просимо поточний документ
#define TOPIC_SHADOW_GET_ACCEPTED "$aws/things/" THING_NAME "/shadow/get/accepted"  // <- документ
#define TOPIC_SHADOW_GET_REJECTED "$aws/things/" THING_NAME "/shadow/get/rejected"  // <- 404: shadow ще не існує
#define TOPIC_SHADOW_UPDATE       "$aws/things/" THING_NAME "/shadow/update"        // -> звітуємо reported
#define TOPIC_SHADOW_DELTA        "$aws/things/" THING_NAME "/shadow/update/delta"  // <- desired змінився

// межі для interval_sec із Shadow: значення поза ними ігноруємо
#define MIN_INTERVAL_SEC 10
#define MAX_INTERVAL_SEC 3600

// документ роботи з presigned URL — це не 200 байтів; з буфером 256 він мовчки обріжеться
#define MQTT_BUFFER_SIZE 4096
#define OTA_MIN_SIZE 100000   // менше — це точно не прошивка
#define HTTP_TIMEOUT 15000

// Deep Sleep: скільки максимум слухаємо відповідь (jobs/$next/get, shadow/get) після запиту
#define MQTT_WAIT_TIMEOUT 2000
#define WAKE_MQTT_ATTEMPTS 3   // спроб підключитись за одне пробудження — далі спимо, щоб не з'їсти батарею

// Timer intervals
#define PUBLISH_INTERVAL 60000   // за замовчуванням, поки Shadow не сказав інакше
// експоненційний retry Wi-Fi/MQTT: 1 → 2 → 4 → 8 → 16 → 30 с (максимум)
#define RECONNECT_BASE_DELAY 1000
#define RECONNECT_MAX_DELAY  30000
#define DEBOUNCE_DELAY 30
#define NTP_TIMEOUT 10000

// Any epoch below this means NTP has not synced yet (ESP32 boots in 1970)
#define NTP_VALID_EPOCH 1700000000

//  Pin defines
#define SWITCH_PIN 27   // RTC GPIO — тільки такі можуть будити з Deep Sleep (GPIO5 — не може)
#define SENSOR_DHT_PIN 4
#define LIGHT_SENSOR_PIN 34   // ADC1 (працює разом з Wi-Fi), лише вхід
#define ADC_MAX 4095          // 12-бітний АЦП
#define DHTT_TYPE DHT22
#define LED_PIN 2


/***    Structs           ***/
typedef struct {
  float humidity;
  float temperature;
  uint8_t status;
} s_dhtData;

/***    Global variables    ***/
uint32_t lastReconectionAttempt = 0;
uint32_t reconectionAttempts = 0;
uint32_t lastPublish = 0;
bool firstPublishDone = false;   // false = publish right after (re)connecting
uint8_t g_buttonState = BUTTON_LOGIC_LOW;

// OTA: колбек лише копіює документ роботи, а виконує його loop()
volatile bool jobPending = false;
volatile bool otaInProgress = false;
char jobBuf[MQTT_BUFFER_SIZE];
bool appConfirmed = false;       // нова прошивка вже підтвердила себе (rollback скасовано)
bool jobsRequested = false;      // після підключення вже спитали jobs/$next/get

// Shadow: поточна конфігурація пристрою
bool shadowRequested = false;    // після підключення вже спитали shadow/get
bool shadowReceived = false;     // Deep Sleep: відповідь на shadow/get уже прийшла

// ═══ ЩО ПЕРЕЖИВАЄ СОН: RTC_DATA_ATTR ═══
// Deep Sleep вимикає основну RAM — звичайні змінні після пробудження знову мають початкові значення.
// RTC-пам'ять (8 КБ) живе між пробудженнями, але не переживає втрату живлення і RESET.
// Ініціалізація тут спрацьовує лише при подачі живлення, не після сну
RTC_DATA_ATTR bool ledOn = false;
RTC_DATA_ATTR uint32_t publishInterval = PUBLISH_INTERVAL;   // мс, змінюється через desired.interval_sec
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint32_t uptimeBeforeSleepSec = 0;   // скільки пристрій «жив» до цього пробудження (з урахуванням сну)
// Після OTA нова прошивка прокидається з RTC-пам'яттю старої — і читає її за іншими адресами.
// Версія в RTC не збіглась = це перший старт цієї прошивки, RTC-змінні треба скинути
RTC_DATA_ATTR char rtcFirmwareVersion[16] = "";

/***    Global objects      ***/
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);
DHT dht(SENSOR_DHT_PIN, DHTT_TYPE);

/***    Function declarations   ***/
bool connectWifi(void);
void configureTLS(void);
bool connectMqtt(void);
bool syncNtpTime(void);
time_t currentTimestamp(void);
void checkButton(uint8_t buttonPin);
bool isWifiConnected(void);
s_dhtData get_dhtData(void);
void publishReading(void);
void publishSensorData(float temperature, float humidity, uint16_t light);
uint16_t readLightLevel(void);
uint32_t reconnectDelay(uint32_t attempts);
void publishManualTrigger(void);
uint32_t uptimeSec(void);
bool initRtcState(void);
void mqttWait(uint32_t timeoutMs, volatile bool *doneFlag);
bool connectForWakeCycle(void);
void runWakeCycle(bool wokeByButton);
void goToSleep(void);
void publishSensorError(void);
void onMessage(char *topic, byte *payload, unsigned int length);
void publishLedEvent(bool ledOn);
void bootBlink(void);
void processJob(void);
void reportJobStatus(const char *jobId, const char *status);
bool downloadAndFlash(const char *url);
bool writeFirmware(Stream &stream, size_t length);
void rebootDevice(void);
void setLed(bool on);
void handleShadow(const char *topic, byte *payload, unsigned int length);
bool applyShadowState(JsonObjectConst state);
void reportShadow(void);

// Arduino за замовчуванням підтверджує новий образ ще ДО setup() — тоді rollback
// захищає лише від падіння на старті. true = підтвердимо самі, після успішного MQTT
extern "C" bool verifyRollbackLater() { return true; }


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
  pinMode(LIGHT_SENSOR_PIN, INPUT);
  analogReadResolution(12);   // 0..4095
  if (freshBoot) {
    bootBlink();              // блимаємо лише при першому старті прошивки, а не щохвилини після сну
  }
  digitalWrite(LED_PIN, ledOn ? HIGH : LOW);   // стан LED пережив сон у RTC
  dht.begin();
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

void onMessage(char *topic, byte *payload, unsigned int length){
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

// Єдине місце, де змінюється LED: і для commands/led, і для Shadow
void setLed(bool on) {
  ledOn = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  Serial.println(on ? "[LED] Увімкнено" : "[LED] Вимкнено");

  // confirm only after the command was actually executed
  publishLedEvent(on);
}

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

// Модуль фоторезистора в Wokwi: що світліше, то нижча напруга на AO.
// Перевертаємо, щоб light_level ріс разом з освітленістю: 0 = темно, 4095 = дуже світло
uint16_t readLightLevel(void) {
  return ADC_MAX - analogRead(LIGHT_SENSOR_PIN);
}

// attempts = скільки спроб підряд уже не вдалося: 0 → 1 с, 1 → 2 с, 2 → 4 с ... максимум 30 с
uint32_t reconnectDelay(uint32_t attempts) {
  if (attempts > 5) {
    attempts = 5;   // 1000 << 5 = 32 с — далі зсувати немає сенсу, все одно обріжемо до 30
  }
  uint32_t delayMs = RECONNECT_BASE_DELAY << attempts;
  return (delayMs > RECONNECT_MAX_DELAY) ? RECONNECT_MAX_DELAY : delayMs;
}

bool isWifiConnected() { return WiFi.status() == WL_CONNECTED; }

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

void checkButton(uint8_t buttonPin) {

  static uint8_t currentSavedButtonState = digitalRead(buttonPin);
  static uint32_t debounceStartTime = 0;
  static uint8_t debounceStarted = 0;

  uint8_t buttonState = digitalRead(buttonPin);

  // Detect state transition
  if (buttonState != currentSavedButtonState && !debounceStarted) {
    debounceStartTime = millis();
    debounceStarted = 1;
  }

  // Check the state again after the debounce delay
  if (debounceStarted && (millis() - debounceStartTime >= DEBOUNCE_DELAY)) {
    if (buttonState != currentSavedButtonState) {
      currentSavedButtonState = buttonState;
      if (buttonState == BUTTON_LOGIC_HIGH) {
        g_buttonState = BUTTON_LOGIC_HIGH;
        Serial.println("\nButton was pressed");
      } else {
        g_buttonState = BUTTON_LOGIC_LOW;
      }
    }
    // Reset debounce flag regardless of press or release
    debounceStarted = 0;
  }
  return;
}

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
void bootBlink(void) {
  for (int i = 0; i < BOOT_BLINKS; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
}

/***    OTA через AWS IoT Jobs    ***/

// Те, що приходить на $next/get/accepted, — обгортка навколо нашого job.json:
// {"execution":{"jobId":"...","status":"QUEUED","jobDocument":{"operation":"ota","version":"1.1.0","url":"https://..."}}}
void processJob(void) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jobBuf);
  if (err) {
    Serial.print("[OTA] Невалідний JSON: ");
    Serial.println(err.c_str());   // IncompleteInput = замалий буфер
    return;
  }

  // роботи немає — поля execution просто не буде
  if (!doc["execution"].is<JsonObject>()) {
    Serial.println("[OTA] Робіт немає");
    return;
  }

  // рядки з doc вказують усередину jobBuf — jobId копіюємо в свій буфер
  char jobId[65];
  strlcpy(jobId, doc["execution"]["jobId"] | "", sizeof(jobId));
  const char *op      = doc["execution"]["jobDocument"]["operation"] | "";
  const char *version = doc["execution"]["jobDocument"]["version"] | "";
  const char *url     = doc["execution"]["jobDocument"]["url"] | "";

  Serial.print("[OTA] Робота ");
  Serial.print(jobId);
  Serial.print(": ");
  Serial.print(FIRMWARE_VERSION);
  Serial.print(" -> ");
  Serial.println(version);

  if (strcmp(op, "ota") != 0 || url[0] == '\0') {
    Serial.println("[OTA] Невідома операція або немає url");
    reportJobStatus(jobId, "FAILED");
    return;
  }

  // Після перезавантаження та сама робота приходить ще раз (вона IN_PROGRESS).
  // Версія вже збігається — оновлення справді відбулось
  if (strcmp(version, FIRMWARE_VERSION) == 0) {
    Serial.println("[OTA] Вже на цій версії -> SUCCEEDED");
    reportJobStatus(jobId, "SUCCEEDED");
    return;
  }

  reportJobStatus(jobId, "IN_PROGRESS");   // остання можливість щось сказати

  // Два TLS-з'єднання одночасно — заявка на брак пам'яті. Рвемо MQTT самі
  mqttClient.disconnect();
  wifiClient.stop();                        // саме stop() — інакше витік mbedTLS

  if (downloadAndFlash(url)) {
    rebootDevice();                         // MQTT більше не потрібен
  }

  // не вдалось — піднімаємось назад на старій прошивці і звітуємо
  Serial.println("[OTA] Оновлення не вдалося, працюємо далі на старій прошивці");
  if (connectMqtt()) {
    reportJobStatus(jobId, "FAILED");
  }
}

// Топік звіту містить jobId всередині себе — ось навіщо ми його зберігали
void reportJobStatus(const char *jobId, const char *status) {
  char topic[128];
  snprintf(topic, sizeof(topic), "$aws/things/%s/jobs/%s/update", THING_NAME, jobId);

  char payload[128];
  snprintf(payload, sizeof(payload),
      "{\"status\":\"%s\",\"statusDetails\":{\"firmware_version\":\"%s\"}}",
      status, FIRMWARE_VERSION);

  bool ok = mqttClient.publish(topic, payload);
  Serial.print("[OTA] Статус ");
  Serial.print(status);
  Serial.println(ok ? " - OK" : " - помилка публікації");
}

// Presigned URL живе обмежений час (60–3600 с) — використовуємо його одразу
bool downloadAndFlash(const char *url) {
  WiFiClientSecure client;
  client.setCACert(root_ca);   // S3 підписаний тим самим Amazon Root CA 1, що й IoT Core

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT);
  if (!http.begin(client, url)) {
    Serial.println("[OTA] Невалідний URL");
    return false;
  }

  Serial.println("[OTA] Завантажуємо прошивку...");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    // 403 — URL прострочений або в ролі presigning немає s3:GetObject
    Serial.print("[OTA] HTTP-код: ");
    Serial.println(code);
    http.end();
    return false;
  }

  // ═══ ЗАПОБІЖНИК: це взагалі прошивка? ═══
  int len = http.getSize();
  if (len < OTA_MIN_SIZE) {
    Serial.print("[OTA] Замало байтів (");
    Serial.print(len);
    Serial.println("). Це не прошивка");
    http.end();
    return false;
  }

  // потік з відповіді сервера — те саме джерело байтів для writeStream()
  bool ok = writeFirmware(*http.getStreamPtr(), len);
  http.end();
  return ok;
}

// ═══ ЗАПИС ПРОШИВКИ У ВІЛЬНИЙ СЛОТ ═══
// Ми ніде не вказуємо номер слота і не чіпаємо otadata вручну — це робить Update
bool writeFirmware(Stream &stream, size_t length) {
  // резервуємо місце; слот вибирає бібліотека
  if (!Update.begin(length)) {
    Update.printError(Serial);
    return false;
  }

  Serial.println("[OTA] Пишемо прошивку...");
  size_t written = Update.writeStream(stream);
  if (written != length) {
    Serial.print("[OTA] Записано байтів: ");
    Serial.println(written);
    Update.abort();           // звільняємо слот, нічого не перемикаємо
    return false;
  }

  // перевірка образу і — тільки після неї — запис в otadata
  if (!Update.end(true)) {
    Update.printError(Serial);
    return false;
  }
  return true;
}

void rebootDevice(void) {
  Serial.println("[OTA] Готово. Перезавантаження...");
  delay(500);   // даємо Serial дописати
  // У Wokwi після ESP.restart() нова прошивка може зависнути на старті.
  // Рестарт через deep sleep на 100 мс працює і в Wokwi, і на справжній платі
  esp_sleep_enable_timer_wakeup(100 * 1000);
  esp_deep_sleep_start();
}

/***    Device Shadow    ***/

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
bool applyShadowState(JsonObjectConst state) {
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

/***    Deep Sleep    ***/

// millis() після кожного пробудження починає з нуля — додаємо час, прожитий до сну (разом зі сном)
uint32_t uptimeSec(void) {
  return uptimeBeforeSleepSec + millis() / 1000;
}

// Повертає true, якщо це перший старт цієї прошивки (живлення, RESET або щойно після OTA)
bool initRtcState(void) {
  if (strcmp(rtcFirmwareVersion, FIRMWARE_VERSION) == 0) {
    return false;   // прокинулись після сну тієї самої прошивки — RTC-змінні справжні
  }
  ledOn = false;
  publishInterval = PUBLISH_INTERVAL;
  bootCount = 0;
  uptimeBeforeSleepSec = 0;
  strlcpy(rtcFirmwareVersion, FIRMWARE_VERSION, sizeof(rtcFirmwareVersion));
  return true;
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

// Те саме зростання пауз, що й у loop(), але з обмеженням: не вийшло за кілька спроб — спимо далі
bool connectForWakeCycle(void) {
  for (uint32_t attempt = 0; attempt < WAKE_MQTT_ATTEMPTS; attempt++) {
    if (!isWifiConnected()) {
      connectWifi();
    }
    if (isWifiConnected() && connectMqtt()) {
      return true;
    }
    uint32_t pause = reconnectDelay(attempt);
    Serial.print("[Retry] Наступна спроба через ");
    Serial.print(pause / 1000);
    Serial.println(" с");
    delay(pause);   // тут можна: setup() — єдина програма, слухати поки нічого
  }
  return false;
}

// Чекліст пробудження (Заняття 16)
void runWakeCycle(bool wokeByButton) {
  if (wokeByButton) {
    Serial.println("[Sleep] Розбудила кнопка");
  }

  // 1-2. мережа + NTP (уже в setup) і MQTT + підписки
  if (!connectForWakeCycle()) {
    Serial.println("[Sleep] Не вдалося підключитись - спимо до наступного разу");
    goToSleep();
  }

  // 3. чи є оновлення? Синхронно: OTA робимо прямо тут, ДО сну
  jobPending = false;
  mqttClient.publish(TOPIC_JOBS_GET, "{}");
  mqttWait(MQTT_WAIT_TIMEOUT, &jobPending);
  if (jobPending) {
    jobPending = false;
    otaInProgress = true;
    processJob();             // успіх → перезавантаження всередині, сюди не повернемось
    otaInProgress = false;    // невдача → повернулись і працюємо далі
  }

  // 4. віддали вимір (і подію, якщо розбудила кнопка)
  publishReading();
  if (wokeByButton) {
    publishManualTrigger();
  }

  // 5. який стан має бути? delta/документ прийшов → виконали; тиша → усе вже правильно
  shadowReceived = false;
  mqttClient.publish(TOPIC_SHADOW_GET, "{}");
  mqttWait(MQTT_WAIT_TIMEOUT, &shadowReceived);
  mqttWait(200, nullptr);     // дати reported/телеметрії піти в мережу до відключення

  // 6. запобіжник: не спимо, поки йде запис прошивки
  if (otaInProgress) {
    return;
  }
  goToSleep();
}

void goToSleep(void) {
  uint32_t sleepSec = publishInterval / 1000;   // interval_sec із Shadow пережив сон у RTC
  uptimeBeforeSleepSec += millis() / 1000 + sleepSec;

  // акуратно закриваємо MQTT і TLS — брокер одразу знає, що нас немає
  mqttClient.disconnect();
  wifiClient.stop();
  WiFi.disconnect(true);

  // таймер: прокинутись через interval_sec
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);

  // кнопка: будить одразу (ext0, рівень 0 = натиснута). Внутрішня підтяжка має жити і під час сну
  rtc_gpio_pullup_en((gpio_num_t)SWITCH_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)SWITCH_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)SWITCH_PIN, 0);

  // LED тримає свій стан (з Shadow / commands) і під час сну
  digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
  gpio_hold_en((gpio_num_t)LED_PIN);
  gpio_deep_sleep_hold_en();

  Serial.print("[Sleep] Deep Sleep на ");
  Serial.print(sleepSec);
  Serial.println(" с");
  Serial.flush();
  esp_deep_sleep_start();     // керування сюди не повернеться
}
