#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "secrets.h"

/***    Defines   ***/

// ═══ ВЕРСІЯ ЦІЄЇ ПРОШИВКИ ═══
// Зібрали нову прошивку — підняли число. Забули — OTA-job ніколи не завершиться.
#define FIRMWARE_VERSION "1.0.0"
// Видима зміна між версіями: скільки разів LED блимає при старті.
// 1.0.0 -> 1 раз, 1.1.0 -> 3 рази
#define BOOT_BLINKS 1

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

// документ роботи з presigned URL — це не 200 байтів; з буфером 256 він мовчки обріжеться
#define MQTT_BUFFER_SIZE 4096
#define OTA_MIN_SIZE 100000   // менше — це точно не прошивка
#define HTTP_TIMEOUT 15000

// Timer intervals
#define PUBLISH_INTERVAL 30000
#define RECONNECT_INTERVAL 5000
#define DEBOUNCE_DELAY 30
#define NTP_TIMEOUT 10000

// Any epoch below this means NTP has not synced yet (ESP32 boots in 1970)
#define NTP_VALID_EPOCH 1700000000

//  Pin defines
#define SWITCH_PIN 5
#define SENSOR_DHT_PIN 4
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
void publishSensorData(float temperature, float humidity);
void publishSensorError(void);
void onMessage(char *topic, byte *payload, unsigned int length);
void publishLedEvent(bool ledOn);
void bootBlink(void);
void processJob(void);
void reportJobStatus(const char *jobId, const char *status);
bool downloadAndFlash(const char *url);
bool writeFirmware(Stream &stream, size_t length);
void rebootDevice(void);

// Arduino за замовчуванням підтверджує новий образ ще ДО setup() — тоді rollback
// захищає лише від падіння на старті. true = підтвердимо самі, після успішного MQTT
extern "C" bool verifyRollbackLater() { return true; }


void setup() {
  Serial.begin(115200);
  Serial.print("Старт, прошивка ");
  Serial.println(FIRMWARE_VERSION);
  pinMode(SWITCH_PIN,INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  bootBlink();
  dht.begin();
  if (!connectWifi()) {
    // no blocking retry here - loop() keeps reconnecting every RECONNECT_INTERVAL
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
  if (isWifiConnected()) {
    connectMqtt();
  }
}

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
    // reconnect without delay(), forever - one attempt per RECONNECT_INTERVAL
    if ((now - lastReconectionAttempt) > RECONNECT_INTERVAL) {
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
    }
    return;
  }

  wasConnected = true;
  mqttClient.loop();
  reconectionAttempts = 0;

  // документ роботи прийшов у колбек — виконуємо тут, а не в колбеку:
  // завантаження триває десятки секунд, а колбек має швидко повернутись
  if (jobPending) {
    jobPending = false;
    otaInProgress = true;
    processJob();             // успіх = перезавантаження, сюди вже не повернемось
    otaInProgress = false;    // сюди потрапляємо лише якщо не вдалося
    return;
  }

  // first reading right after connecting, then every PUBLISH_INTERVAL
  if (!firstPublishDone || (now - lastPublish) > PUBLISH_INTERVAL) {
    firstPublishDone = true;
    lastPublish = now;
    publishReading();
  }

  // button not checked during disconnect - no broker to publish to
  checkButton(SWITCH_PIN);
  static uint8_t manual_published = 0;

  if (g_buttonState == BUTTON_LOGIC_HIGH) {
    // one extra reading per button press, without waiting for the timer
    if (manual_published == 0) {
      publishReading();
      manual_published++;
    }
  } else {
    manual_published = 0;
  }
}

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

    // Нова прошивка довела, що працює: Wi-Fi, TLS і MQTT піднялись.
    // Тільки тепер скасовуємо rollback. Без цього перший же reset поверне стару версію
    if (!appConfirmed) {
      esp_ota_mark_app_valid_cancel_rollback();
      appConfirmed = true;
      Serial.println("[OTA] Прошивку підтверджено (rollback скасовано)");
    }

    // роботу могли створити, поки ми були офлайн — ніхто нам про неї не скаже, питаємо самі.
    // Після OTA цей самий запит поверне нашу роботу ще раз — і ми звітуємо SUCCEEDED
    mqttClient.publish(TOPIC_JOBS_GET, "{}");
    return true;
  }

  Serial.print("Помилка: ");
  Serial.println(mqttClient.state());
  return false;
}

void onMessage(char *topic, byte *payload, unsigned int length){
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

  // expected: {"action":"set","value":"on"} or {"action":"set","value":"off"}
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, message, length);
  if (err) {
    Serial.print("[LED] Невалідний JSON: ");
    Serial.println(err.c_str());
    return;
  }

  const char *action = doc["action"] | "";  // "" if the field is missing
  const char *value  = doc["value"]  | "";

  if (strcmp(action, "set") != 0) {
    Serial.print("[LED] Невідома дія: ");
    Serial.println(action);
    return;
  }

  bool ledOn;
  if (strcmp(value, "on") == 0) {
    ledOn = true;
  } else if (strcmp(value, "off") == 0) {
    ledOn = false;
  } else {
    Serial.print("[LED] Невідоме значення: ");
    Serial.println(value);
    return;
  }

  digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
  Serial.println(ledOn ? "[LED] Увімкнено" : "[LED] Вимкнено");

  // confirm only after the command was actually executed
  publishLedEvent(ledOn);
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
  publishSensorData(dht_data.temperature, dht_data.humidity);
}

void publishSensorData(float temperature, float humidity) {
    if (!mqttClient.connected()) {
        Serial.println("[MQTT] Не підключено — пропускаємо");
        return;
    }

    time_t now = currentTimestamp();

    // snprintf обріже рядок, якщо буфер замалий — JSON стане битим,
    // і правило його не розбере. 160 байт вистачає з запасом
    char payload[160];
    snprintf(payload, sizeof(payload),
        "{\"device_id\":\"%s\",\"timestamp\":%lu,"
        "\"temperature\":%.1f,\"humidity\":%.1f}",
        DEVICE_PAYLOAD_ID, (unsigned long)now, temperature, humidity);

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