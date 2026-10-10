from typing import Literal

from botocore.exceptions import ClientError
from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from dotenv import load_dotenv

load_dotenv()          # ← до імпорту db/iot, щоб змінні вже були в середовищі
import db
import iot

app = FastAPI(title="IoT Backend")

# ═══════════════ CORS ═══════════════
# HTML-сторінка відкривається з іншого origin (file:// або інший порт),
# і без цього браузер заблокує її fetch() до API
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],          # локальна домашка; у проді — конкретний домен
    allow_methods=["GET", "POST"],
    allow_headers=["Content-Type"],
)


# ═══════════════ Схема тіла запиту ═══════════════
# Literal — FastAPI сам відхилить усе, крім цих значень (422 Unprocessable Entity)
class LedCommand(BaseModel):
    action: Literal["set"]
    value: Literal["on", "off"]


@app.get("/health")
def health():
    return {"status": "ok"}

@app.get("/sensors/latest")
def sensors_latest():
    item = db.get_latest()
    if item is None:
        raise HTTPException(status_code=404, detail="No data yet")
    return item

@app.get("/sensors/history")
def sensors_history(minutes: int = Query(30, ge=1, le=1440)):   # 1 хв … 24 год
    return db.get_history(minutes)

@app.get("/events")
def events(limit: int = Query(20, ge=1, le=100)):
    return db.get_events(limit)

@app.post("/actuators/led")
def actuators_led(command: LedCommand):
    try:
        iot.publish_led_command(command.model_dump())
    except ClientError as e:
        code = e.response["Error"]["Code"]
        if code == "ForbiddenException":
            # AWS не пояснює причину — підказуємо самі
            raise HTTPException(status_code=502,
                                detail="ForbiddenException: add iot:Publish to the backend IAM user")
        raise HTTPException(status_code=502, detail=f"{code}: {e.response['Error'].get('Message')}")
    return {"status": "sent", "topic": iot.COMMAND_TOPIC, "command": command.model_dump()}
