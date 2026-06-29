from fastapi import FastAPI, Request
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
import asyncio

from hub_control import main
from state import system_state

app = FastAPI()

app.mount("/static", StaticFiles(directory="static"), name="static")
templates = Jinja2Templates(directory="templates")

@app.on_event("startup")
async def startup_event():
    asyncio.create_task(main())

@app.get("/api/state")
async def get_state():
    return system_state

@app.get("/")
async def dashboard(request: Request):
    return templates.TemplateResponse(request, "index.html")


