"""
FastAPI + WebSocket - Dashboard telemetría Supervisor de Bombas
"""

import asyncio
import json
from contextlib import asynccontextmanager
from datetime import datetime
from typing import List, Set

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, HTMLResponse
from pydantic import BaseModel

from .state import StateManager
from .models import SystemStatus, Alert, SystemState, McuCommand
from .alerts import AlertManager


# Instancias globales
state_manager = StateManager()
alert_manager = AlertManager()
active_websockets: Set[WebSocket] = set()


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup
    print("[API] Iniciando telemetría...")
    if not state_manager.start():
        print("[API] ADVERTENCIA: No se pudo conectar al MCU")
    
    # Registrar callbacks
    state_manager.on_status_change = on_status_change
    state_manager.on_alert = on_alert
    
    # Task de heartbeat
    asyncio.create_task(heartbeat_task())
    
    yield
    
    # Shutdown
    print("[API] Cerrando telemetría...")
    state_manager.stop()


app = FastAPI(
    title="Supervisor de Bombas - Telemetría",
    version="1.0.0",
    lifespan=lifespan
)

# Servir archivos estáticos
app.mount("/static", StaticFiles(directory="static"), name="static")


class CommandRequest(BaseModel):
    command: str  # "set_modo_generador", "set_mantenimiento", "trigger_emergencia", "reset_emergencia"
    params: dict = {}


async def heartbeat_task():
    """Enviar heartbeat periódico a WebSockets y solicitar estado al MCU"""
    while True:
        await asyncio.sleep(5)
        
        # Broadcast estado actual
        if active_websockets:
            data = {
                "type": "heartbeat",
                "timestamp": datetime.now().isoformat(),
                "status": state_manager.status.to_dict(),
                "stats": state_manager.get_stats()
            }
            await broadcast(json.dumps(data))
        
        # Solicitar estado al MCU cada 30s
        if datetime.now().second % 30 == 0:
            state_manager.request_status()


def on_status_change(status: SystemStatus):
    """Callback cuando cambia el estado del sistema"""
    asyncio.create_task(broadcast_status(status))


def on_alert(alert: Alert):
    """Callback cuando hay una alerta"""
    alert_manager.add(alert)
    asyncio.create_task(broadcast_alert(alert))


async def broadcast_status(status: SystemStatus):
    data = {
        "type": "status",
        "timestamp": datetime.now().isoformat(),
        "status": status.to_dict()
    }
    await broadcast(json.dumps(data))


async def broadcast_alert(alert: Alert):
    data = {
        "type": "alert",
        "timestamp": datetime.now().isoformat(),
        "alert": alert.to_dict()
    }
    await broadcast(json.dumps(data))


async def broadcast(message: str):
    """Enviar mensaje a todos los WebSockets conectados"""
    disconnected = set()
    for ws in active_websockets:
        try:
            await ws.send_text(message)
        except Exception:
            disconnected.add(ws)
    
    for ws in disconnected:
        active_websockets.discard(ws)


# ============================================================
# Rutas HTTP
# ============================================================

@app.get("/", response_class=HTMLResponse)
async def root():
    return FileResponse("static/index.html")


@app.get("/api/status")
async def get_status():
    return state_manager.status.to_dict()


@app.get("/api/stats")
async def get_stats():
    return state_manager.get_stats()


@app.get("/api/alerts")
async def get_alerts(limit: int = 100):
    return [a.to_dict() for a in alert_manager.get_recent(limit)]


@app.post("/api/command")
async def send_command(req: CommandRequest):
    success = False
    message = ""
    
    if req.command == "set_modo_generador":
        generador = req.params.get("generador", False)
        success = state_manager.set_modo_generador(generador)
        message = f"Modo {'GENERADOR' if generador else 'RED'} solicitado"
        
    elif req.command == "set_mantenimiento":
        activo = req.params.get("activo", False)
        success = state_manager.set_mantenimiento(activo)
        message = f"Modo mantenimiento {'activado' if activo else 'desactivado'}"
        
    elif req.command == "trigger_emergencia":
        codigo = req.params.get("codigo", 0xFF00)
        success = state_manager.trigger_emergencia(codigo)
        message = f"Emergencia activada (código: {codigo:04X})"
        
    elif req.command == "reset_emergencia":
        success = state_manager.reset_emergencia()
        message = "Emergencia reseteada"
        
    elif req.command == "request_status":
        success = state_manager.request_status()
        message = "Estado solicitado"
        
    else:
        raise HTTPException(400, f"Comando desconocido: {req.command}")
    
    return {"success": success, "message": message}


# ============================================================
# WebSocket
# ============================================================

@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    active_websockets.add(ws)
    print(f"[WS] Cliente conectado. Total: {len(active_websockets)}")
    
    # Enviar estado inicial
    await ws.send_text(json.dumps({
        "type": "init",
        "timestamp": datetime.now().isoformat(),
        "status": state_manager.status.to_dict(),
        "alerts": [a.to_dict() for a in alert_manager.get_recent(50)],
        "stats": state_manager.get_stats()
    }))
    
    try:
        while True:
            data = await ws.receive_text()
            try:
                msg = json.loads(data)
                if msg.get("type") == "ping":
                    await ws.send_text(json.dumps({"type": "pong"}))
                elif msg.get("type") == "command":
                    # Permitir comandos vía WS también
                    await handle_ws_command(ws, msg)
            except json.JSONDecodeError:
                pass
    except WebSocketDisconnect:
        pass
    finally:
        active_websockets.discard(ws)
        print(f"[WS] Cliente desconectado. Total: {len(active_websockets)}")


async def handle_ws_command(ws: WebSocket, msg: dict):
    cmd = msg.get("command")
    params = msg.get("params", {})
    
    if cmd == "set_modo_generador":
        state_manager.set_modo_generador(params.get("generador", False))
    elif cmd == "set_mantenimiento":
        state_manager.set_mantenimiento(params.get("activo", False))
    elif cmd == "trigger_emergencia":
        state_manager.trigger_emergencia(params.get("codigo", 0xFF00))
    elif cmd == "reset_emergencia":
        state_manager.reset_emergencia()
    elif cmd == "request_status":
        state_manager.request_status()
    
    await ws.send_text(json.dumps({"type": "ack", "command": cmd}))


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8080)