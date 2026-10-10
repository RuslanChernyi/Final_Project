import os
import json
import boto3

# ═══════════════ Підключення до AWS IoT (data plane) ═══════════════
# "iot-data" — це не той самий "iot", що керує Things/Policy.
# Публікація йде на Device data endpoint — той самий, до якого підключається ESP32
IOT_ENDPOINT = os.getenv("IOT_ENDPOINT")
COMMAND_TOPIC = os.getenv("COMMAND_TOPIC")

iot = boto3.client(
    "iot-data",
    region_name=os.getenv("AWS_DEFAULT_REGION"),
    endpoint_url=f"https://{IOT_ENDPOINT}",
)

# ═══════════════ Команди на пристрій ═══════════════
def publish_led_command(command: dict):
    """Публікує команду в топік, на який підписаний ESP32."""
    iot.publish(
        topic=COMMAND_TOPIC,             # iot-course/Ruslan_Chernyi/commands/led
        qos=1,                           # брокер підтвердить отримання
        payload=json.dumps(command),     # {"action": "set_led", "value": "on"}
    )


# ═══════════════ Device Shadow ═══════════════
# Ім'я Thing збігається з device_id: esp32-Ruslan_Chernyi
THING_NAME = os.getenv("DEVICE_ID")

def get_shadow():
    """Повний документ Shadow: state.desired / state.reported / state.delta."""
    response = iot.get_thing_shadow(thingName=THING_NAME)
    return json.loads(response["payload"].read())   # payload — потік байтів, а не dict


def update_desired(desired: dict):
    """Записує desired. Якщо він відрізняється від reported — AWS надішле пристрою delta."""
    response = iot.update_thing_shadow(
        thingName=THING_NAME,
        payload=json.dumps({"state": {"desired": desired}}),
    )
    return json.loads(response["payload"].read())
