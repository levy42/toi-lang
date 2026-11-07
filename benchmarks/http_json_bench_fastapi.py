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


@app.get("/api")
async def api():
    return PAYLOAD
