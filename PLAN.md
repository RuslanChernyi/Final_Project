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

## Крок 1. Прошивка: формат телеметрії і поведінка (14 балів) — готово

Файли: `firmware/src/main.cpp`, `firmware/diagram.json`.

- [x] Додати фоторезистор `wokwi-photoresistor-sensor` у `diagram.json` (AO → D34, ADC1) і читати `analogRead()` → `light_level`.
- [x] Розширити payload телеметрії до формату з вимог: `light_level`, `firmware_version` (`"1.0.0"`),
      `uptime_sec` (`millis()/1000`), `free_heap` (`ESP.getFreeHeap()`). (`firmware_version` уже додано в кроці 6.)
- [x] Інтервал публікації `PUBLISH_INTERVAL` 30 с → 60 с.
- [x] Кнопка: крім позачергової телеметрії, публікувати в `events`
      `{"device_id","timestamp","event":"manual_trigger","firmware_version"}`.
- [x] Експоненційний retry замість фіксованих 5 с: 1 → 2 → 4 → 8 → 16 → 30 с (максимум), скидати до 1 с після успіху.
      Логіка вже є в `loop()` через `RECONNECT_INTERVAL` — достатньо рахувати затримку з `reconectionAttempts`.
- [x] Команда LED: `"action":"set"` → `"action":"set_led"` (бекенд `LedCommand` і `web/index.html` оновлені теж).

Перевірка: Serial показує новий JSON раз на 60 с, у DynamoDB з'являються нові поля, натискання кнопки
дає запис `manual_trigger` в `iot_events`.

## Крок 2. AWS: три правила і документи в `aws/` (14 балів)

- [ ] `OverheatAlert`: `WHERE temperature > 28` → `WHERE temperature > 28 OR temperature < 15`
      (можна перейменувати на `TemperatureAlarm`). Оновити `aws/rules/OverheatAlert.json`.
- [ ] Звірити JSON-и в `aws/` з тим, що реально налаштовано в консолі (вони записані з README домашки).
- [x] Політика Thing: додати топіки Shadow і Jobs (крок 5–6) — лише для свого Thing:
      `$aws/things/esp32-Ruslan_Chernyi/shadow/*` (`jobs/*` уже додано в кроці 6)
      (`topic/` для Publish і Receive, `topicfilter/` для Subscribe).
- [ ] Скріншот CloudWatch з алармом (температура в Wokwi > 28 і < 15) → `docs/screenshots/`.

## Крок 3. Бекенд: 7 ендпоінтів (11 балів) — готово (лишилось оновити IAM-політику в консолі)

Файли: `backend/main.py`, `backend/db.py`, `backend/iot.py`.

- [x] `/sensors/history?minutes=N` → `?hours=N` (1–24), за замовчуванням 6.
- [x] `/events?limit=N` — за замовчуванням 10 (для таблиці в Grafana); за бажанням фільтр лише `manual_trigger`.
- [x] `LedCommand.action`: `Literal["set"]` → `Literal["set_led"]`; оновити `web/index.html`.
- [x] Два нові ендпоінти для Shadow (уточнити у викладача, які саме очікуються; найлогічніше):
  - `GET /shadow` — `iot-data.get_thing_shadow()` → `reported` / `desired` / `delta`;
  - `PUT /shadow/desired` — `iot-data.update_thing_shadow()` з `{"led":"on","interval_sec":60}`.
- [x] IAM-політика бекенду: додати `iot:GetThingShadow`, `iot:UpdateThingShadow` на `thing/esp32-Ruslan_Chernyi`
      → `aws/backend-iam-policy.json`.
- [x] Почистити `requirements.txt` до прямих залежностей (fastapi, uvicorn, boto3, python-dotenv).

## Крок 4. Grafana: чотири панелі з вимог (6 балів)

- [ ] Time series: температура **і** вологість за 6 год (`/sensors/history?hours=6`), діапазон дашборду `Last 6 hours`.
- [ ] Gauge: `light_level` з `/sensors/latest`, кольорові зони (червона — темно, зелена — норма).
- [ ] Stat: час останньої синхронізації (`received_at`, UNIX ms) і `uptime_sec`.
- [ ] Table: останні 10 подій `manual_trigger` (`/events?limit=10`).
- [ ] Експортувати JSON у `grafana/dashboard.json`, скріншот → `docs/screenshots/`.

## Крок 5. Device Shadow (8 балів) — прошивка готова

Зроблено в `firmware/src/main.cpp`: підписки на `shadow/get/accepted`, `shadow/get/rejected`, `shadow/update/delta`;
явний `shadow/get` після кожного підключення; `desired.led` (`on`/`off`) і `desired.interval_sec` (10–3600, поза межами — ігнор);
`reported` = `led`, `interval_sec`, `firmware_version`; 404 на get → створюємо Shadow своїм reported;
`commands/led` теж оновлює reported — пріоритет у Shadow (delta повертає LED до desired).

Лишилось:
- [x] Policy пристрою: `$aws/things/esp32-Ruslan_Chernyi/shadow/*` — `topic/` для Publish і Receive, `topicfilter/` для Subscribe
      (без цього AWS розірве з'єднання на першому ж `shadow/get`, state=-3);
- [ ] перевірка в консолі: **Things → esp32-Ruslan_Chernyi → Device Shadows → Classic** → змінити `desired`;
- [x] бекенд: `GET /shadow`, `PUT /shadow/desired` + IAM `iot:GetThingShadow`, `iot:UpdateThingShadow` (крок 3);
- [ ] README: розділ про Shadow і обмеження «Shadow має пріоритет над commands/led».

## Крок 6. OTA через AWS IoT Jobs, 1.0.0 → 1.1.0 (12 балів) — код готовий

Зроблено (за Заняттям 15): `partitions.csv` з двома слотами, `merged.bin` для Wokwi (автоматично, `merge_bin.py`),
`FIRMWARE_VERSION` у телеметрії, `BOOT_BLINKS` як видима зміна, підписки на Jobs, `$next/get` після кожного підключення,
колбек-диспетчер + `jobPending` у `loop()`, звіти `IN_PROGRESS` / `SUCCEEDED` / `FAILED`, відключення MQTT перед
завантаженням, `Update.begin/writeStream/end`, `verifyRollbackLater()` + підтвердження після MQTT, рестарт через deep sleep 100 мс,
`setBufferSize(4096)`, документи `aws/thing-policy.json` (jobs/*), `aws/job-ota-1.1.0.json`, `aws/iam/ota-presign-role-*.json`.

Лишилось зробити руками в AWS (покроково — README, розділ 8):
- [x] оновити Policy пристрою в консолі до `aws/thing-policy.json`;
- [ ] зібрати 1.1.0 (`FIRMWARE_VERSION "1.1.0"`, `BOOT_BLINKS 3`), покласти `firmware_1_1_0.bin` у приватний S3-бакет, повернути 1.0.0;
- [ ] завантажити `job-ota-1.1.0.json` (з назвою свого бакета) у S3, створити роль `iot_ota_presign_role`;
- [ ] створити job `ota-1-1-0`, пройти цикл у Wokwi, перевірити `SUCCEEDED` і `firmware_version: 1.1.0` у DynamoDB;
- [ ] скріншоти: статус job, Serial-лог оновлення → `docs/screenshots/`.

## Крок 7. Безпека і документація (5 + 4 бали)

- [ ] README: архітектурна діаграма (оновити під Shadow/Jobs/CloudWatch), відтворення з нуля,
      відомі обмеження своїми словами: **QoS 0** (PubSubClient не публікує з QoS 1), пріоритет Shadow над `commands/led`,
      чи робили Deep Sleep.
- [ ] Скріншоти: DynamoDB (5+ записів), CloudWatch, Grafana, job → `docs/screenshots/`.
- [ ] Перед першим `git push`: `git status` — переконатися, що `secrets.h`, `.env`, `*.pem` не потрапили в індекс.
- [ ] Відео до 2 хв: змінити температуру в Wokwi → графік у Grafana → кнопка LED → LED у Wokwi.

## Крок 8 (додатково). Deep Sleep (6 балів) — прошивка готова

Окрема збірка `pio run -e esp32dev_deepsleep` (`ENABLE_DEEP_SLEEP=1`), базова лишається завжди онлайн.
Чекліст пробудження із Заняття 16: Wi-Fi → MQTT → jobs (OTA до сну) → телеметрія → Shadow → `otaInProgress`? → сон на `interval_sec`.
`RTC_DATA_ATTR`: LED, інтервал, `bootCount`, `uptime_sec`, версія прошивки (скидання після OTA). Кнопку перенесено D5 → D27 (RTC GPIO, ext0).

Лишилось:
- [ ] прогнати в Wokwi (переключити `wokwi.toml` на `esp32dev_deepsleep`): сон 60 с, пробудження кнопкою, `uptime_sec` росте;
- [ ] змінити `desired.interval_sec` у Shadow → пристрій спить новий інтервал;
- [ ] README: у розділі «Відомі обмеження» — «так, Deep Sleep зроблено як окремий режим».

## Підводні камені (слайд Заняття 19)

| Симптом | Лікування | Стан |
|---|---|---|
| Не компілюється | `platform = espressif32@6.13.0` | ✅ |
| Команди не приходять після reconnect | підписки всередині `connectMqtt()` | ✅ |
| У DynamoDB один запис | `received_at` з `timestamp()` | ✅ |
| Shadow не бачить стан | явний `shadow/get` на старті | крок 5 |
| Shadow відповідає, колбек мовчить | `setBufferSize(3072)` | ✅ (4096, крок 6) |
| Колбек мовчить | `iot:Receive` у політиці | ✅ (Jobs додано; Shadow — крок 5) |
| Після OTA стара версія | `esp_ota_mark_app_valid_cancel_rollback()` | ✅ |
| Grafana в 1970 році | `received_at` як UNIX ms | ✅ |

## Довідка

Попередня згенерована версія проєкту збережена в `../Final_Project_generated_backup/` — там є готові
приклади Shadow (`src/shadow.cpp`), OTA (`src/ota.cpp`), Deep Sleep (`src/power.cpp`) і документів AWS.
Можна підглядати, але переписувати у стилі власного коду.
