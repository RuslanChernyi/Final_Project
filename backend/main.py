from typing import Literal, Optional

from botocore.exceptions import ClientError
from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field, model_validator
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
    allow_methods=["GET", "POST", "PUT"],
    allow_headers=["Content-Type"],
)


# ═══════════════ Схема тіла запиту ═══════════════
# Literal — FastAPI сам відхилить усе, крім цих значень (422 Unprocessable Entity)
class LedCommand(BaseModel):
    action: Literal["set_led"]
    value: Literal["on", "off"]


# Межі interval_sec ті самі, що в прошивці (MIN/MAX_INTERVAL_SEC): поза ними пристрій ігнорує значення
class ShadowDesired(BaseModel):
    led: Optional[Literal["on", "off"]] = None
    interval_sec: Optional[int] = Field(None, ge=10, le=3600)

    @model_validator(mode="after")
    def not_empty(self):
        if self.led is None and self.interval_sec is None:
            raise ValueError("Вкажіть led і/або interval_sec")
        return self


def aws_error(e: ClientError, hint: str) -> HTTPException:
    """Помилку AWS — у 502 з підказкою; ForbiddenException AWS не пояснює, тож пояснюємо самі."""
    code = e.response["Error"]["Code"]
    if code == "ForbiddenException":
        return HTTPException(status_code=502, detail=f"ForbiddenException: {hint}")
    return HTTPException(status_code=502, detail=f"{code}: {e.response['Error'].get('Message')}")


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
def sensors_history(hours: int = Query(6, ge=1, le=24)):   # 1 … 24 год
    return db.get_history(hours)

@app.get("/events")
def events(limit: int = Query(10, ge=1, le=100),
           event: Optional[str] = Query(None, examples=["manual_trigger"])):   # без event — усі події
    return db.get_events(limit, event)

@app.post("/actuators/led")
def actuators_led(command: LedCommand):
    try:
        iot.publish_led_command(command.model_dump())
    except ClientError as e:
        raise aws_error(e, "add iot:Publish to the backend IAM user")
    return {"status": "sent", "topic": iot.COMMAND_TOPIC, "command": command.model_dump()}

@app.get("/shadow")
def shadow():
    try:
        document = iot.get_shadow()
    except ClientError as e:
        # Shadow ще не створений: пристрій жодного разу не підключався з новою прошивкою
        if e.response["Error"]["Code"] == "ResourceNotFoundException":
            raise HTTPException(status_code=404, detail="Shadow not found")
        raise aws_error(e, "add iot:GetThingShadow to the backend IAM user")
    state = document.get("state", {})
    return {
        "desired": state.get("desired", {}),
        "reported": state.get("reported", {}),
        "delta": state.get("delta", {}),    # що пристрій ще не виконав
        "version": document.get("version"),
    }

@app.put("/shadow/desired")
def shadow_desired(desired: ShadowDesired):
    try:
        document = iot.update_desired(desired.model_dump(exclude_none=True))
    except ClientError as e:
        raise aws_error(e, "add iot:UpdateThingShadow to the backend IAM user")
    return {"status": "accepted", "desired": document["state"]["desired"], "version": document.get("version")}
