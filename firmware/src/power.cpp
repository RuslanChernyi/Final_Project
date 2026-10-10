#include "power.h"

#include <Arduino.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include "config.h"
#include "network.h"
#include "ota.h"
#include "shadow.h"
#include "telemetry.h"

// Ініціалізація тут спрацьовує лише при подачі живлення, не після сну
RTC_DATA_ATTR bool ledOn = false;
RTC_DATA_ATTR uint32_t publishInterval = PUBLISH_INTERVAL;
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint32_t uptimeBeforeSleepSec = 0;   // скільки пристрій «жив» до цього пробудження (з урахуванням сну)
// Після OTA нова прошивка прокидається з RTC-пам'яттю старої — і читає її за іншими адресами.
// Версія в RTC не збіглась = це перший старт цієї прошивки, RTC-змінні треба скинути
RTC_DATA_ATTR char rtcFirmwareVersion[16] = "";

static bool connectForWakeCycle(void);

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

// Те саме зростання пауз, що й у loop(), але з обмеженням: не вийшло за кілька спроб — спимо далі
static bool connectForWakeCycle(void) {
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

  sleepSec = 1;
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
