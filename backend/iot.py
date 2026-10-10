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
        payload=json.dumps(command),     # {"action": "set", "value": "on"}
    )
