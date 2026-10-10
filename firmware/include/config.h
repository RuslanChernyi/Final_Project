#ifndef CONFIG_H
#define CONFIG_H

// Усі налаштування прошивки в одному місці: версія, мережа, топіки, інтервали, піни

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

#endif // CONFIG_H
