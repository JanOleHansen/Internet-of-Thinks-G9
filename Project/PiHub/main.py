from fastapi import FastAPI
import asyncio

app = FastAPI()

@app.get("/BLE_Dashboard")
def dashboard():
    return "Welcome to your BLE Smart Home Dashboard!"



