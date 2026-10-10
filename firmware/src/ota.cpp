#include "ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include "network.h"

volatile bool jobPending = false;
volatile bool otaInProgress = false;
char jobBuf[MQTT_BUFFER_SIZE];
bool appConfirmed = false;
bool jobsRequested = false;

static void reportJobStatus(const char *jobId, const char *status);
static bool downloadAndFlash(const char *url);
static bool writeFirmware(Stream &stream, size_t length);
static void rebootDevice(void);

// Arduino за замовчуванням підтверджує новий образ ще ДО setup() — тоді rollback
// захищає лише від падіння на старті. true = підтвердимо самі, після успішного MQTT
extern "C" bool verifyRollbackLater() { return true; }

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
static void reportJobStatus(const char *jobId, const char *status) {
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
static bool downloadAndFlash(const char *url) {
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
static bool writeFirmware(Stream &stream, size_t length) {
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

static void rebootDevice(void) {
  Serial.println("[OTA] Готово. Перезавантаження...");
  delay(500);   // даємо Serial дописати
  // У Wokwi після ESP.restart() нова прошивка може зависнути на старті.
  // Рестарт через deep sleep на 100 мс працює і в Wokwi, і на справжній платі
  esp_sleep_enable_timer_wakeup(100 * 1000);
  esp_deep_sleep_start();
}
