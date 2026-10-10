# Курсовий проєкт: моніторинг мікроклімату на ESP32 + AWS IoT

> **Стан:** базова версія = домашнє завдання до Заняття 14. Що лишилося зробити — у [PLAN.md](PLAN.md).

ESP32 + AWS IoT Core + DynamoDB + FastAPI + Grafana. ESP32 (Wokwi, DHT22) шле телеметрію через MQTT/TLS в AWS IoT Core,
Rules Engine складає її в DynamoDB, FastAPI віддає дані в Grafana. У зворотний бік:
HTML-сторінка → FastAPI → AWS IoT → ESP32 вмикає/вимикає світлодіод і підтверджує це подією.

## Архітектура

```text
                       ┌──────────────────────────────────┐
                       │  ESP32 (Wokwi)                   │
                       │  DHT22 → D4, кнопка → D5,        │
                       │  LED → D2 (через 220 Ω)          │
                       └───┬──────────────────────────▲───┘
      ① телеметрія, 30 с   │                          │  ④ команда
   .../telemetry           │  MQTT/TLS :8883          │  .../commands/led
   ⑤ підтвердження         │                          │  {"action":"set","value":"on"}
   .../events              │                          │
   {"event":"led_changed"} ▼                          │
┌──────────────────────────────────────────────────────┴───────────────────┐
│  AWS IoT Core (eu-central-1)                                             │
│                                                                          │
│  Rules Engine:                                                           │
│    StoreTelemetry ── dynamoDBv2 ──► DynamoDB iot_telemetry               │
│    OverheatAlert (temp > 28) ─────► CloudWatch /aws/iot/rules/overheat   │
│    StoreEvents (.../events) ─ dynamoDBv2 ─► DynamoDB iot_events          │
│    (error action у всіх трьох правил → /aws/iot/rules/errors)            │
└───────────────┬──────────────────────────────────────▲───────────────────┘
                │                                      │ ③ iot-data Publish (HTTPS)
                ▼                                      │
        ┌──────────────────┐   ② Query        ┌────────┴───────────────────┐
        │  DynamoDB        │ ───────────────► │  FastAPI  :8000            │
        │  iot_telemetry   │                  │  GET  /sensors/latest      │
        │  iot_events      │                  │  GET  /sensors/history     │
        └──────────────────┘                  │  GET  /events              │
                                              │  POST /actuators/led       │
                                              └──▲─────────────────▲───────┘
                                     GET (JSON)  │                 │ POST (fetch)
                                  ┌──────────────┴──────┐   ┌──────┴──────────────┐
                                  │ Grafana :3000       │   │ web/index.html      │
                                  │ Infinity datasource │   │ кнопки Увімк/Вимк   │
                                  └─────────────────────┘   └─────────────────────┘
```

- **Дані вгору (①②):** ESP32 → `telemetry` → правило `StoreTelemetry` → DynamoDB → FastAPI → Grafana.
- **Команди вниз (③④):** кнопка на сторінці → `POST /actuators/led` → `iot-data:Publish` → `commands/led` → ESP32 перемикає LED.
- **Підтвердження (⑤):** після `digitalWrite` ESP32 публікує `led_changed` у `events` → правило `StoreEvents` →
  таблиця `iot_events` → `GET /events` → панель Table у Grafana.

## Структура репозиторію

```text
firmware/                    PlatformIO-проєкт ESP32
  platformio.ini             збірка, запінена платформа espressif32@6.13.0 і бібліотеки
  src/main.cpp               прошивка
  include/example.secrets.h  шаблон сертифікатів → скопіювати в include/secrets.h
  diagram.json, wokwi.toml   схема і конфіг Wokwi
backend/                     FastAPI
  main.py                    ендпоінти, CORS
  db.py                      запити до DynamoDB
  iot.py                     публікація команд в AWS IoT
  requirements.txt           залежності Python
  .env.example               шаблон змінних середовища (без реальних значень)
aws/                         документи AWS
  thing-policy.json          політика Thing
  backend-iam-policy.json    inline policy IAM-користувача бекенду
  rules/*.json               три IoT Rules (StoreTelemetry, StoreEvents, OverheatAlert)
grafana/dashboard.json       експорт дашборду (JSON Model)
web/index.html               сторінка з кнопками керування LED
docs/screenshots/            скріншоти для здачі
```

## Параметри

| Що | Значення |
|---|---|
| Регіон | `eu-central-1` (усі ресурси в одному регіоні) |
| Thing / Client ID / `device_id` | `esp32-Ruslan_Chernyi` |
| Телеметрія (пристрій → хмара) | `iot-course/Ruslan_Chernyi/telemetry` — `{"device_id","timestamp","temperature","humidity"}` |
| Команди (хмара → пристрій) | `iot-course/Ruslan_Chernyi/commands/led` — `{"action":"set","value":"on"\|"off"}` |
| Події (пристрій → хмара) | `iot-course/Ruslan_Chernyi/events` — `{"event":"led_changed","value":"on"}` або `{"error":"dht_read_failed"}` |
| Таблиці DynamoDB | `iot_telemetry` і `iot_events` (обидві: `device_id` String + `received_at` Number, мс) |

## Створені ресурси AWS

| Тип | Назва | Призначення |
|---|---|---|
| IoT Thing | `esp32-Ruslan_Chernyi` | пристрій, до нього прикріплений сертифікат |
| IoT Certificate | (auto-generated) | mTLS-автентифікація ESP32 |
| IoT Policy | політика пристрою (JSON нижче) | Connect лише своїм Client ID, Publish/Subscribe/Receive лише в `iot-course/Ruslan_Chernyi/*` |
| IoT Rule | `StoreTelemetry` | телеметрія → DynamoDB, error action → CloudWatch |
| IoT Rule | `OverheatAlert` | `temperature > 28` → CloudWatch, error action → CloudWatch |
| IoT Rule | `StoreEvents` | події → DynamoDB `iot_events`, error action → CloudWatch |
| DynamoDB table | `iot_telemetry` | телеметрія, On-demand |
| DynamoDB table | `iot_events` | події пристрою (`led_changed`, `dht_read_failed`), On-demand |
| IAM role | `iot_rule_ddb_role` | дозволяє `StoreTelemetry` робити `dynamodb:PutItem` в `iot_telemetry` |
| IAM role | `iot_rule_events_ddb_role` | дозволяє `StoreEvents` робити `dynamodb:PutItem` в `iot_events` |
| IAM role | `iot_rule_cw_role` | дозволяє правилам писати в CloudWatch Logs |
| CloudWatch log groups | `/aws/iot/rules/errors`, `/aws/iot/rules/overheat` | помилки правил і алерти перегріву |
| IAM user | `iot-course-backend` | під ним працює FastAPI (ключі в `backend/.env`) |

## Налаштування з нуля

### 1. AWS IoT: Thing, сертифікат, Policy

1. Перевірити регіон **eu-central-1** у правому верхньому куті консолі.
2. **AWS IoT Core → Manage → Things → Create thing** → `esp32-Ruslan_Chernyi`.
3. **Auto-generate certificate** → завантажити `certificate.pem.crt`, `private.pem.key`, `AmazonRootCA1.pem`.
4. Створити і прикріпити до сертифіката Policy (тільки власний Client ID і власні топіки):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow", "Action": "iot:Connect",
      "Resource": "arn:aws:iot:eu-central-1:<ACCOUNT_ID>:client/esp32-Ruslan_Chernyi" },
    { "Effect": "Allow", "Action": "iot:Publish",
      "Resource": "arn:aws:iot:eu-central-1:<ACCOUNT_ID>:topic/iot-course/Ruslan_Chernyi/*" },
    { "Effect": "Allow", "Action": "iot:Subscribe",
      "Resource": "arn:aws:iot:eu-central-1:<ACCOUNT_ID>:topicfilter/iot-course/Ruslan_Chernyi/*" },
    { "Effect": "Allow", "Action": "iot:Receive",
      "Resource": "arn:aws:iot:eu-central-1:<ACCOUNT_ID>:topic/iot-course/Ruslan_Chernyi/*" }
  ]
}
```

`Subscribe` + `Receive` покривають і `commands/led`, тож для команд окремих змін у політиці не треба.

5. Device data endpoint: **IoT Core → Settings** (вигляд `xxxx-ats.iot.eu-central-1.amazonaws.com`).
   Він потрібен і прошивці (`DOMAIN_ADDRESS` у `firmware/src/main.cpp`), і бекенду (`IOT_ENDPOINT` у `.env`).

### 2. DynamoDB і правила

Таблиця `iot_telemetry`: partition key `device_id` (**String**), sort key `received_at` (**Number**), Default settings (On-demand).

Правило `StoreTelemetry`, SQL version `2016-03-23`:

```sql
SELECT *,
       timestamp() AS received_at,
       clientid()  AS client_id,
       topic(2)    AS student
FROM 'iot-course/Ruslan_Chernyi/telemetry'
```

- Дія: **DynamoDBv2** → `iot_telemetry`, роль `iot_rule_ddb_role` (Create new role)
- Error action: **CloudWatch Logs** → `/aws/iot/rules/errors`, роль `iot_rule_cw_role`

Правило `OverheatAlert`:

```sql
SELECT device_id, temperature, humidity, timestamp() AS received_at
FROM 'iot-course/Ruslan_Chernyi/telemetry'
WHERE temperature > 28
```

- Дія: **CloudWatch Logs** → `/aws/iot/rules/overheat`
- Error action: **CloudWatch Logs** → `/aws/iot/rules/errors`

#### Події: таблиця `iot_events` і правило `StoreEvents`

Таблиця `iot_events`: ті самі ключі — `device_id` (**String**) + `received_at` (**Number**), On-demand.

Правило `StoreEvents`:

```sql
SELECT *,
       timestamp() AS received_at,
       clientid()  AS client_id
FROM 'iot-course/Ruslan_Chernyi/events'
```

- Дія: **DynamoDBv2** → `iot_events`, роль **Create new role** → `iot_rule_events_ddb_role`
  (роль `iot_rule_ddb_role` дозволяє `PutItem` лише в `iot_telemetry`, тож для нової таблиці — нова роль)
- Error action: **CloudWatch Logs** → `/aws/iot/rules/errors`, роль `iot_rule_cw_role`

У таблицю потрапляють усі події: `{"event":"led_changed","value":"on"}` і `{"error":"dht_read_failed"}`.

#### Error action: як перевірити, що він працює

Error action спрацьовує, коли правило **спрацювало, але дія впала** (не коли повідомлення не підійшло під `WHERE`).
Без нього такі повідомлення зникають мовчки: ESP32 бачить `publish OK`, у таблиці — порожньо, помилок — ніде.

1. У `StoreEvents` тимчасово прибрати рядок `timestamp() AS received_at,` → **Update**.
2. Натиснути кнопку на HTML-сторінці. У MQTT test client подія є, у `iot_events` нового запису немає.
3. **CloudWatch → Log groups → `/aws/iot/rules/errors`** → повідомлення з `ruleName`, `failures[].errorMessage`
   (немає sort key `received_at`) і `base64OriginalPayload` — втрачений payload.
4. Повернути рядок назад → **Update**.

### 3. IAM-користувач для бекенду

**IAM → Users → Create user** → `iot-course-backend` → **Security credentials → Create access key**
(ключі підуть у `backend/.env`). Inline policy — лише читання своїх таблиць і публікація лише у свої команди:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow", "Action": "dynamodb:Query",
      "Resource": [
        "arn:aws:dynamodb:eu-central-1:<ACCOUNT_ID>:table/iot_telemetry",
        "arn:aws:dynamodb:eu-central-1:<ACCOUNT_ID>:table/iot_events"
      ] },
    { "Effect": "Allow", "Action": "iot:Publish",
      "Resource": "arn:aws:iot:eu-central-1:<ACCOUNT_ID>:topic/iot-course/Ruslan_Chernyi/commands/*" }
  ]
}
```

Без `iot:Publish` ендпоінт `POST /actuators/led` поверне `502 ForbiddenException`.

### 4. Прошивка ESP32

```bash
cd firmware
cp include/example.secrets.h include/secrets.h   # вставити вміст трьох .pem файлів
pio run                                          # збірка
```

Симуляція: VS Code + розширення **Wokwi** → `F1 → Wokwi: Select Config File` → `firmware/wokwi.toml`, потім `F1 → Wokwi: Start Simulator`. Serial Monitor — `115200`.

Бібліотеки (ставляться автоматично з `platformio.ini`): `adafruit/DHT sensor library`,
`knolleary/PubSubClient`, `bblanchon/ArduinoJson` — версії запінені.

Що робить прошивка:

- раз на 30 с (і одразу після підключення) читає DHT22 і публікує в `telemetry`;
- кнопка D5 — позачергова публікація;
- підписана на `commands/led` (QoS 1): парсить JSON, на `{"action":"set","value":"on"|"off"}` вмикає/вимикає LED на D2
  і **після** цього публікує `{"device_id","timestamp","event":"led_changed","value"}` у `events`;
  невалідний JSON або невідомі `action`/`value` ігноруються з повідомленням у Serial.

| Збій | Поведінка |
|---|---|
| Wi-Fi не підключився | не блокує старт; `loop()` викликає `WiFi.reconnect()` раз на 5 с |
| NTP не пройшов за 10 с | старт продовжується, `timestamp` = `0`; SNTP досинхронізується у фоні |
| MQTT відвалився | reconnect раз на 5 с без `delay()`, після reconnect — повторна підписка на команди |
| DHT22 повернув NaN | телеметрія не публікується, натомість `{"error":"dht_read_failed"}` у `events` |

### 5. Бекенд (FastAPI)

Потрібен Python 3.10+.

```bash
python -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
pip install -r backend/requirements.txt
```

`backend/.env` (шаблон — `backend/.env.example`):

```ini
AWS_ACCESS_KEY_ID=...                 # ключі IAM-користувача iot-course-backend
AWS_SECRET_ACCESS_KEY=...
AWS_DEFAULT_REGION=eu-central-1
TABLE_NAME=iot_telemetry
EVENTS_TABLE_NAME=iot_events
DEVICE_ID=esp32-Ruslan_Chernyi
IOT_ENDPOINT=xxxx-ats.iot.eu-central-1.amazonaws.com
COMMAND_TOPIC=iot-course/Ruslan_Chernyi/commands/led
```

Запуск:

```bash
cd backend
uvicorn main:app --reload           # http://127.0.0.1:8000, Swagger — /docs
```

| Ендпоінт | Що робить |
|---|---|
| `GET /health` | `{"status":"ok"}` |
| `GET /sensors/latest` | останній запис пристрою з DynamoDB (404, якщо даних немає) |
| `GET /sensors/history?minutes=30` | записи за останні N хвилин (`minutes` 1–1440) |
| `GET /events?limit=20` | останні N подій з `iot_events`, найновіші згори (`limit` 1–100) |
| `POST /actuators/led` | тіло `{"action":"set","value":"on"\|"off"}` → публікація в `commands/led`; інші значення → 422 |

Перевірка через curl:

```bash
curl localhost:8000/sensors/latest
curl "localhost:8000/sensors/history?minutes=30"
curl "localhost:8000/events?limit=5"
curl -X POST localhost:8000/actuators/led \
     -H 'Content-Type: application/json' -d '{"action":"set","value":"on"}'
```

`200 {"status":"sent"}` означає, що брокер прийняв команду. Що LED справді змінився —
підтверджує подія `led_changed` у топіку `events` (видно в **IoT Core → MQTT test client**).

CORS увімкнено (`allow_origins=["*"]`) — інакше браузер заблокує запити з HTML-сторінки.

### 6. HTML-сторінка керування

`web/index.html` — дві кнопки, що шлють `POST /actuators/led`. Адреса API задана в `API_URL` (`http://127.0.0.1:8000`).

```bash
cd web
python -m http.server 5500          # http://localhost:5500
```

Або просто відкрити файл у браузері. Окрема сторінка потрібна, бо в Grafana OSS немає кнопки з POST без додаткових плагінів.

### 7. Grafana

Grafana запущена локально на `http://localhost:3000` (дашборд експортовано з Grafana 13).

1. **Administration → Plugins** → встановити **Infinity** (`yesoreyeram-infinity-datasource`).
2. **Connections → Data sources → Add → Infinity** → Save (додаткових налаштувань не треба — URL задані в панелях).
3. **Dashboards → New → Import** → завантажити `grafana/dashboard.json`.
   Якщо панелі показують «datasource not found» — у кожній панелі **Edit** → обрати свій Infinity datasource.

Панелі (усі — Infinity, Type `JSON`, Parser `Backend`, Method `GET`):

| Панель | Тип | URL | Колонки |
|---|---|---|---|
| Температура за останні 30 хвилин | Time series | `/sensors/history?minutes=30` | `timestamp` (epoch, **s**), `temperature` |
| Поточна вологість | Gauge | `/sensors/latest` | `humidity` |
| Коли зроблений останній вимір | Stat | `/sensors/latest` | `timestamp` (epoch, **s**) — час пристрою |
| Коли був отриманий останній вимір | Stat | `/sensors/latest` | `received_at` (epoch, **ms**) — час AWS |
| Останні події пристрою | Table | `/events?limit=20` | `received_at` (epoch, **ms**), `event`, `value`, `error` |

`timestamp` ставить пристрій у секундах, `received_at` — правило в мілісекундах. Якщо переплутати тип колонки,
дата виявиться в 1970-му або в далекому майбутньому.

Дашборд: діапазон часу `Last 30 minutes`, автооновлення `30s`.

## Порти

| Сервіс | Адреса |
|---|---|
| FastAPI | `http://127.0.0.1:8000` (`/docs` — Swagger) |
| Grafana | `http://localhost:3000` |
| HTML-сторінка | `http://localhost:5500` (або `file://`) |
| AWS IoT (MQTT/TLS) | `<endpoint>:8883` |

## Перевірка всього ланцюжка

1. Wokwi запущено, у Serial — `[MQTT] Публікуємо: ...` і `[MQTT] OK`.
2. `curl localhost:8000/sensors/latest` повертає свіжий запис.
3. Grafana показує графік, вологість і час останнього виміру, оновлюється кожні 30 с.
4. У MQTT test client підписатися на `iot-course/Ruslan_Chernyi/events`.
5. На сторінці натиснути **Увімкнути** → LED у Wokwi світиться, у Serial `[LED] Увімкнено`,
   у test client — `{"event":"led_changed","value":"on"}`.
6. Через ≤ 30 с ця подія з'являється в панелі **Останні події пристрою** (і в `curl localhost:8000/events`).

## Приклад дашборду
<img width="1823" height="962" alt="Screenshot From 2026-10-09 16-48-06" src="https://github.com/user-attachments/assets/82356559-c282-421f-b936-cc47ff726451" />

<img width="1826" height="865" alt="Screenshot From 2026-10-09 16-48-11" src="https://github.com/user-attachments/assets/ec0fcb9a-d3a0-4f84-876a-eadbefca9d45" />

