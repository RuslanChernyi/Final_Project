import os
import time
import boto3
from boto3.dynamodb.conditions import Attr, Key

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


def get_history(hours):
    """Записи пристрою за останні N годин, найстаріші згори (так їх малює графік)."""
    now_ms = int(time.time() * 1000)              # зараз — у мілісекундах
    cutoff_ms = now_ms - hours * 60 * 60 * 1000   # межа: N годин тому

    response = table.query(
        KeyConditionExpression=Key("device_id").eq(DEVICE_ID)
                               & Key("received_at").gte(cutoff_ms),
    )
    return response["Items"]


def get_events(limit, event=None):
    """Останні N подій пристрою — найновіші згори. event="manual_trigger" — лише натискання кнопки."""
    params = {
        "KeyConditionExpression": Key("device_id").eq(DEVICE_ID),
        "ScanIndexForward": False,
    }
    if event is None:
        params["Limit"] = limit
        return events_table.query(**params)["Items"]

    # Limit у DynamoDB рахує записи ДО фільтра: Limit=10 + фільтр може повернути 0 подій,
    # хоча натискання є. Тому фільтруємо і гортаємо сторінки, поки не наберемо limit
    params["FilterExpression"] = Attr("event").eq(event)
    items = []
    while len(items) < limit:
        response = events_table.query(**params)
        items.extend(response["Items"])
        if "LastEvaluatedKey" not in response:
            break
        params["ExclusiveStartKey"] = response["LastEvaluatedKey"]
    return items[:limit]
