"""
FastAPI + WebSocket - Dashboard telemetría Supervisor de Bombas
"""

import asyncio
import json
import os
from contextlib import asynccontextmanager
from datetime import datetime
from typing import List, Optional, Set

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, HTMLResponse
from pydantic import BaseModel

from .state import StateManager
from .models import SystemStatus, Alert, SystemState, McuCommand
from .alerts import AlertManager
from .auth import AuthManager, COMMAND_REQUIRED_ROLE


# Instancias globales
state_manager = StateManager()
# Alertas con persistencia local SQLite (no se pierden datos en caídas de red)
alert_manager = AlertManager(
    db_path=os.environ.get("SUPERVISOR_ALERTS_DB", "supervisor_alerts.db")
)
# Autenticación de comandos (FAIL-CLOSED si no hay contraseñas configuradas)
auth_manager = AuthManager.from_env()
active_websockets: Set[WebSocket] = set()


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup
    print("[API] Iniciando telemetría...")
    if not auth_manager.configured:
        print("[API] ADVERTENCIA: sin SUPERVISOR_OPERATOR_PASSWORD/"
              "SUPERVISOR_ADMIN_PASSWORD -> comandos REMOTOS BLOQUEADOS (fail-closed)")
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


class LoginRequest(BaseModel):
    password: str


def extract_token(request: Request) -> Optional[str]:
    """Token desde 'Authorization: Bearer <t>' o cabecera 'X-Auth-Token'."""
    auth_hdr = request.headers.get("Authorization", "")
    if auth_hdr.lower().startswith("bearer "):
        return auth_hdr[7:].strip()
    return request.headers.get("X-Auth-Token")


@app.post("/api/auth/login")
async def login(req: LoginRequest):
    """Intercambia contraseña por token (operator o admin)."""
    result = auth_manager.login(req.password)
    if not result:
        auth_manager.audit("login", None, False, "credenciales inválidas")
        raise HTTPException(401, "Credenciales inválidas")
    auth_manager.audit("login", result["role"], True)
    return result


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
    # Evaluar umbrales y generar alertas
    for alert in alert_manager.check_thresholds(status):
        alert_manager.add(alert)
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
async def send_command(req: CommandRequest, request: Request):
    # 1. Autenticación (fail-closed: sin token válido, nada se ejecuta)
    role = auth_manager.verify(extract_token(request))
    if role is None:
        auth_manager.audit(req.command, None, False, "sin token válido")
        raise HTTPException(401, "Autenticación requerida")

    # 2. Comando conocido
    if req.command not in COMMAND_REQUIRED_ROLE:
        auth_manager.audit(req.command, role, False, "comando desconocido")
        raise HTTPException(400, f"Comando desconocido: {req.command}")

    # 3. Autorización por rol
    if not auth_manager.authorize(role, req.command):
        auth_manager.audit(req.command, role, False, "rol insuficiente")
        raise HTTPException(403, f"Rol '{role}' no tiene permiso para {req.command}")

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

    # 4. Audit de la ejecución
    auth_manager.audit(req.command, role, success, message)

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
    token = msg.get("token")

    # Autenticación por WebSocket (mismo esquema que HTTP, fail-closed)
    role = auth_manager.verify(token)
    if role is None:
        auth_manager.audit(str(cmd), None, False, "WS sin token válido")
        await ws.send_text(json.dumps({
            "type": "error", "command": cmd,
            "error": "autenticación requerida"
        }))
        return
    if cmd not in COMMAND_REQUIRED_ROLE:
        auth_manager.audit(str(cmd), role, False, "comando desconocido (WS)")
        await ws.send_text(json.dumps({
            "type": "error", "command": cmd, "error": "comando desconocido"
        }))
        return
    if not auth_manager.authorize(role, cmd):
        auth_manager.audit(str(cmd), role, False, "rol insuficiente (WS)")
        await ws.send_text(json.dumps({
            "type": "error", "command": cmd, "error": "permiso denegado"
        }))
        return

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

    auth_manager.audit(cmd, role, True, "WS")
    await ws.send_text(json.dumps({"type": "ack", "command": cmd}))


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8080)