from fastapi import FastAPI, Request
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from fastapi.responses import JSONResponse
import asyncio
from datetime import datetime, timedelta

import hub_control
from hub_control import main, COMMAND_LIGHT_ON, COMMAND_LIGHT_OFF, COMMAND_HEATING_ON, COMMAND_HEATING_OFF
from state import system_state

COMMAND_MAP = {
    ("lighting", "on"):  COMMAND_LIGHT_ON,
    ("lighting", "off"): COMMAND_LIGHT_OFF,
    ("heating",  "on"):  COMMAND_HEATING_ON,
    ("heating",  "off"): COMMAND_HEATING_OFF,
}

app = FastAPI()

app.mount("/static", StaticFiles(directory="static"), name="static")
templates = Jinja2Templates(directory="templates")

@app.on_event("startup")
async def startup_event():
    asyncio.create_task(main())

@app.get("/api/state")
async def get_state():
    return system_state

@app.post("/api/command/{device}/{action}")
async def send_command(device: str, action: str):
    cmd = COMMAND_MAP.get((device, action))
    if cmd is None:
        return JSONResponse({"success": False, "error": "Invalid command"}, status_code=400)
    if hub_control.act_client is None:
        return JSONResponse({"success": False, "error": "Actuator not connected"}, status_code=503)
    try:
        success = await hub_control.send_command_with_retry(hub_control.act_client, cmd)
        if success:
            actuator = system_state["actuator"]
            actuator[device] = action
            actuator[f"{device}_override_until"] = (
                datetime.now() + timedelta(minutes=2)
            ).isoformat()
        return {"success": success}
    except Exception as e:
        return JSONResponse({"success": False, "error": str(e)}, status_code=500)

@app.get("/")
async def dashboard(request: Request):
    return templates.TemplateResponse(request, "index.html")


