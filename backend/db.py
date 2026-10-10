import os
import time
import boto3
from boto3.dynamodb.conditions import Key

# ═══════════════ Підключення до DynamoDB ═══════════════
# Регіон задаємо явно — щоб не залежати від налаштувань aws config
dynamodb = boto3.resource("dynamodb", region_name=os.getenv("AWS_DEFAULT_REGION"))

TABLE_NAME = os.getenv("TABLE_NAME")
EVENTS_TABLE_NAME = os.getenv("EVENTS_TABLE_NAME")
DEVICE_ID = os.getenv("DEVICE_ID")

table = dynamodb.Table(TABLE_NAME) # type: ignore
events_table = dynamodb.Table(EVENTS_TABLE_NAME) # type: ignore

# ═══════════════ Запити до таблиці ═══════════════
def get_latest():
    """Останній замір пристрою: у його партицію, згори, один."""
    response = table.query(
        KeyConditionExpression=Key("device_id").eq(DEVICE_ID), # esp32-Ruslan_Chernyi
        ScanIndexForward=False,   # найновіші згори
        Limit=1,
    )
    items = response["Items"]
    return items[0] if items else None


def get_history(minutes):
    """Записи пристрою за останні N хвилин."""
    now_ms = int(time.time() * 1000)            # зараз — у мілісекундах
    cutoff_ms = now_ms - minutes * 60 * 1000    # межа: N хвилин тому

    response = table.query(
        KeyConditionExpression=Key("device_id").eq(DEVICE_ID)
                               & Key("received_at").gte(cutoff_ms),
    )
    return response["Items"]


def get_events(limit):
    """Останні N подій пристрою (led_changed, dht_read_failed) — найновіші згори."""
    response = events_table.query(
        KeyConditionExpression=Key("device_id").eq(DEVICE_ID),
        ScanIndexForward=False,
        Limit=limit,
    )
    return response["Items"]
