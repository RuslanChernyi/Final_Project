# План курсового проєкту

База — домашнє завдання до Заняття 14 (вже працює: телеметрія → DynamoDB, події → `iot_events`,
алерт перегріву → CloudWatch, FastAPI на 5 ендпоінтів, Grafana, LED з HTML-сторінки).
Нижче — що додати поверх, у порядку, в якому зручно робити: кожен крок окремо збирається і перевіряється.

Бали — з чекліста Заняття 19 (база 74 + Deep Sleep 6 = 80).

## Що вже є з домашки

| Вимога | Стан |
|---|---|
| DHT22, MQTT/TLS, X.509, підписка на `commands/led` | ✅ |
| Підписки всередині `connectMqtt()` (відновлюються після reconnect) | ✅ |
| `received_at` з `timestamp()` у правилах, sort key у мс | ✅ |
| `iot:Receive` у політиці | ✅ |
| Таблиці `iot_telemetry` і `iot_events`, правила `StoreTelemetry`, `StoreEvents`, `OverheatAlert` | ✅ |
| Debounce кнопки | ✅ |
| `/health`, `/sensors/latest`, `/sensors/history`, `/events`, `POST /actuators/led` | ✅ (треба підправити, див. крок 3) |
| Grafana з Infinity, колонки `received_at` в мс | ✅ (панелі змінити, див. крок 4) |
| Сертифікати й `.env` у `.gitignore`, `.env.example` | ✅ |
| Запінена платформа `espressif32@6.13.0` | ✅ (зроблено при перенесенні) |

## Крок 1. Прошивка: формат телеметрії і поведінка (14 балів)

Файли: `firmware/src/main.cpp`, `firmware/diagram.json`.

- [ ] Додати фоторезистор `wokwi-photoresistor-sensor` у `diagram.json` (AO → D34, ADC1) і читати `analogRead()` → `light_level`.
- [ ] Розширити payload телеметрії до формату з вимог: `light_level`, `firmware_version` (`"1.0.0"`),
      `uptime_sec` (`millis()/1000`), `free_heap` (`ESP.getFreeHeap()`). Буфер `payload[160]` → `payload[256]`.
- [ ] Інтервал публікації `PUBLISH_INTERVAL` 30 с → 60 с.
- [ ] Кнопка: крім позачергової телеметрії, публікувати в `events`
      `{"device_id","timestamp","event":"manual_trigger","firmware_version"}`.
- [ ] Експоненційний retry замість фіксованих 5 с: 1 → 2 → 4 → 8 → 16 → 30 с (максимум), скидати до 1 с після успіху.
      Логіка вже є в `loop()` через `RECONNECT_INTERVAL` — достатньо рахувати затримку з `reconectionAttempts`.
- [ ] Команда LED: `"action":"set"` → `"action":"set_led"` (так у вимогах; синхронно з кроком 3).
- [ ] `mqttClient.setBufferSize(3072)` — знадобиться для Shadow і Jobs (документи більші за 256 байт).

Перевірка: Serial показує новий JSON раз на 60 с, у DynamoDB з'являються нові поля, натискання кнопки
дає запис `manual_trigger` в `iot_events`.

## Крок 2. AWS: три правила і документи в `aws/` (14 балів)

- [ ] `OverheatAlert`: `WHERE temperature > 28` → `WHERE temperature > 28 OR temperature < 15`
      (можна перейменувати на `TemperatureAlarm`). Оновити `aws/rules/OverheatAlert.json`.
- [ ] Звірити JSON-и в `aws/` з тим, що реально налаштовано в консолі (вони записані з README домашки).
- [ ] Політика Thing: додати топіки Shadow і Jobs (крок 5–6) — лише для свого Thing:
      `$aws/things/esp32-Ruslan_Chernyi/shadow/*` і `$aws/things/esp32-Ruslan_Chernyi/jobs/*`
      (`topic/` для Publish і Receive, `topicfilter/` для Subscribe).
- [ ] Скріншот CloudWatch з алармом (температура в Wokwi > 28 і < 15) → `docs/screenshots/`.

## Крок 3. Бекенд: 7 ендпоінтів (11 балів)

Файли: `backend/main.py`, `backend/db.py`, `backend/iot.py`.

- [ ] `/sensors/history?minutes=N` → `?hours=N` (1–24), за замовчуванням 6.
- [ ] `/events?limit=N` — за замовчуванням 10 (для таблиці в Grafana); за бажанням фільтр лише `manual_trigger`.
- [ ] `LedCommand.action`: `Literal["set"]` → `Literal["set_led"]`; оновити `web/index.html`.
- [ ] Два нові ендпоінти для Shadow (уточнити у викладача, які саме очікуються; найлогічніше):
  - `GET /shadow` — `iot-data.get_thing_shadow()` → `reported` / `desired` / `delta`;
  - `PUT /shadow/desired` — `iot-data.update_thing_shadow()` з `{"led":"on","interval_sec":60}`.
- [ ] IAM-політика бекенду: додати `iot:GetThingShadow`, `iot:UpdateThingShadow` на `thing/esp32-Ruslan_Chernyi`
      → `aws/backend-iam-policy.json`.
- [ ] Почистити `requirements.txt` до прямих залежностей (fastapi, uvicorn, boto3, python-dotenv).

## Крок 4. Grafana: чотири панелі з вимог (6 балів)

- [ ] Time series: температура **і** вологість за 6 год (`/sensors/history?hours=6`), діапазон дашборду `Last 6 hours`.
- [ ] Gauge: `light_level` з `/sensors/latest`, кольорові зони (червона — темно, зелена — норма).
- [ ] Stat: час останньої синхронізації (`received_at`, UNIX ms) і `uptime_sec`.
- [ ] Table: останні 10 подій `manual_trigger` (`/events?limit=10`).
- [ ] Експортувати JSON у `grafana/dashboard.json`, скріншот → `docs/screenshots/`.

## Крок 5. Device Shadow (8 балів)

Конфігурація живе в хмарі: `desired.led` (`on`/`off`) і `desired.interval_sec`.

- [ ] Підписатися в `connectMqtt()` на `.../shadow/get/accepted` і `.../shadow/update/delta`.
- [ ] Після підключення — явний `publish(".../shadow/get", "")` (одна лише delta не покаже стан, змінений, поки пристрій був офлайн).
- [ ] Застосувати `led` і `interval_sec`, потім опублікувати `reported` у `.../shadow/update`.
- [ ] Команда з `commands/led` теж оновлює `reported`. Пріоритет — у Shadow: якщо `desired.led` відрізняється,
      прийде delta і поверне LED у бажаний стан. Описати це в README як відоме обмеження.

## Крок 6. OTA через AWS IoT Jobs, 1.0.0 → 1.1.0 (12 балів)

- [ ] S3-бакет з `firmware-1.1.0.bin`; документ job → `aws/job-ota-1.1.0.json`
      (`{"operation":"ota_update","version":"1.1.0","url":"${aws:iot:s3-presigned-url:...}"}`).
- [ ] Прошивка: підписка на `.../jobs/notify-next` і `.../jobs/$next/get/accepted`, на старті — `.../jobs/$next/get`.
- [ ] Отримали job з новою версією → `IN_PROGRESS` → завантаження `esp_https_ota()` → reboot.
- [ ] Після reboot: `esp_ota_mark_app_valid_cancel_rollback()`, потім job знову приходить — версія вже збігається → `SUCCEEDED`.
- [ ] Видима зміна в 1.1.0 (наприклад, LED блимає 3 рази при старті) і `firmware_version` у телеметрії.
- [ ] Задокументувати весь цикл у README зі скріншотом статусу job.

## Крок 7. Безпека і документація (5 + 4 бали)

- [ ] README: архітектурна діаграма (оновити під Shadow/Jobs/CloudWatch), відтворення з нуля,
      відомі обмеження своїми словами: **QoS 0** (PubSubClient не публікує з QoS 1), пріоритет Shadow над `commands/led`,
      чи робили Deep Sleep.
- [ ] Скріншоти: DynamoDB (5+ записів), CloudWatch, Grafana, job → `docs/screenshots/`.
- [ ] Перед першим `git push`: `git status` — переконатися, що `secrets.h`, `.env`, `*.pem` не потрапили в індекс.
- [ ] Відео до 2 хв: змінити температуру в Wokwi → графік у Grafana → кнопка LED → LED у Wokwi.

## Крок 8 (додатково). Deep Sleep (6 балів)

Робити в останню чергу, окремим `[env]` у `platformio.ini`, щоб базова версія залишалась без сну:
прокинувся → підключився → Shadow/Jobs → телеметрія → `esp_deep_sleep_start()` на `interval_sec`;
кнопка як джерело пробудження (`ext0`); `uptime_sec` зберігати в `RTC_DATA_ATTR`.

## Підводні камені (слайд Заняття 19)

| Симптом | Лікування | Стан |
|---|---|---|
| Не компілюється | `platform = espressif32@6.13.0` | ✅ |
| Команди не приходять після reconnect | підписки всередині `connectMqtt()` | ✅ |
| У DynamoDB один запис | `received_at` з `timestamp()` | ✅ |
| Shadow не бачить стан | явний `shadow/get` на старті | крок 5 |
| Shadow відповідає, колбек мовчить | `setBufferSize(3072)` | крок 1 |
| Колбек мовчить | `iot:Receive` у політиці | ✅ (додати Shadow/Jobs — крок 2) |
| Після OTA стара версія | `esp_ota_mark_app_valid_cancel_rollback()` | крок 6 |
| Grafana в 1970 році | `received_at` як UNIX ms | ✅ |

## Довідка

Попередня згенерована версія проєкту збережена в `../Final_Project_generated_backup/` — там є готові
приклади Shadow (`src/shadow.cpp`), OTA (`src/ota.cpp`), Deep Sleep (`src/power.cpp`) і документів AWS.
Можна підглядати, але переписувати у стилі власного коду.
