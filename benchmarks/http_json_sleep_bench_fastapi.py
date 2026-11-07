import asyncio

from fastapi import FastAPI

app = FastAPI()

PAYLOAD = {
    "name": "Ada",
    "active": True,
    "score": 98.5,
    "tags": ["toi", "json", "api"],
    "nested": {"retries": 3, "timeout_ms": 250, "ok": True},
    "users": [
        {"id": 1, "role": "admin"},
        {"id": 2, "role": "staff"},
        {"id": 3, "role": "member"},
    ],
}


@app.get("/api/sleep")
async def api_sleep():
    await asyncio.sleep(0.01)
    return PAYLOAD
