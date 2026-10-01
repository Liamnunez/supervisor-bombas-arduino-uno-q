"""
FastAPI + WebSocket - Dashboard telemetria Supervisor de Bombas

## Endpoints

### Estado y Telemetria
- **GET /api/status** -- Estado actual completo del sistema
- **GET /api/stats** -- Estadisticas de comunicacion MCU
- **GET /api/alerts** -- Alertas recientes (limite configurable)
- **GET /api/history** -- Historial de alertas por horas
- **GET /api/export/csv** -- Exportar alertas a CSV
- **GET /api/bot/stats** -- Estado del Telegram Bot

### Configuracion (requiere autenticacion)
- **GET /api/config/thresholds** -- Obtener umbrales actuales
- **POST /api/config/thresholds** -- Actualizar umbrales (solo admin)

### Comandos (requieren autenticacion + rol)
- **POST /api/command** -- Ejecutar comando (ver modelos abajo)
- **POST /api/auth/login** -- Obtener token JWT (8h)

### WebSocket
- **GET /ws** -- Stream en tiempo real (status, alerts, heartbeat)

## Modelos de Comando

{
  "command": "set_modo_generador|set_mantenimiento|trigger_emergencia|reset_emergencia|request_status",
  "params": {}
}

## Autenticacion
- **Operator**: reset_emergencia, set_mantenimiento, request_status
- **Admin**: todo lo anterior + set_modo_generador, trigger_emergencia
- **Fail-closed**: sin contrasenas configuradas -> ningun comando remoto
"""

import asyncio
import csv
import io
import json
import os
from contextlib import asynccontextmanager
from datetime import datetime
from typing import List, Optional, Set

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.openapi.utils import get_openapi
from pydantic import BaseModel

from .state import StateManager
from .models import SystemStatus, Alert, SystemState, McuCommand
from .alerts import AlertManager
from .auth import AuthManager, COMMAND_REQUIRED_ROLE
from .telegram_bot import create_bot_from_env, TelegramBot


# Instancias globales
state_manager = StateManager()
# Alertas con persistencia local SQLite (no se pierden datos en caidas de red)
alert_manager = AlertManager(
    db_path=os.environ.get("SUPERVISOR_ALERTS_DB", "supervisor_alerts.db")
)
# Autenticacion de comandos (FAIL-CLOSED si no hay contrasenas configuradas)
auth_manager = AuthManager.from_env()
# Telegram Bot (opcional, se inicia si hay token/chat_id en entorno)
telegram_bot: Optional[TelegramBot] = None
active_websockets: Set[WebSocket] = set()


@asynccontextmanager
async def lifespan(app: FastAPI):
    global telegram_bot
    # Startup
    print("[API] Iniciando telemetria...")
    if not auth_manager.configured:
        print("[API] ADVERTENCIA: sin SUPERVISOR_OPERATOR_PASSWORD/"
              "SUPERVISOR_ADMIN_PASSWORD -> comandos REMOTOS BLOQUEADOS (fail-closed)")
    if not state_manager.start():
        print("[API] ADVERTENCIA: No se pudo conectar al MCU")
    
    # Registrar callbacks
    state_manager.on_status_change = on_status_change
    state_manager.on_alert = on_alert
    
    # Telegram Bot (opcional)
    telegram_bot = create_bot_from_env(alert_manager)
    if telegram_bot and telegram_bot.start():
        print("[API] Telegram Bot iniciado")
    elif telegram_bot:
        print("[API] ADVERTENCIA: Telegram Bot no pudo iniciar")
    else:
        print("[API] Telegram Bot no configurado (TELEGRAM_BOT_TOKEN/CHAT_ID)")
    
    # Task de heartbeat
    asyncio.create_task(heartbeat_task())
    
    yield
    
    # Shutdown
    print("[API] Cerrando telemetria...")
    if telegram_bot:
        telegram_bot.stop()
    state_manager.stop()


app = FastAPI(
    title="Supervisor de Bombas - Telemetria",
    version="1.0.0",
    lifespan=lifespan,
    description=__doc__,
    contact={"name": "Supervisor de Bombas", "url": "https://github.com/Liamnunez/supervisor-bombas-arduino-uno-q"},
    license_info={"name": "Internal Use", "url": "https://github.com/Liamnunez/supervisor-bombas-arduino-uno-q"},
    openapi_tags=[
        {"name": "telemetria", "description": "Estado, alertas, historial, exportacion"},
        {"name": "configuracion", "description": "Umbrales y configuracion (auth requerido)"},
        {"name": "comandos", "description": "Comandos de control (auth + rol requerido)"},
        {"name": "auth", "description": "Autenticacion y tokens"},
        {"name": "websocket", "description": "Stream en tiempo real"},
        {"name": "bot", "description": "Telegram Bot"},
    ]
)

def custom_openapi():
    if app.openapi_schema:
        return app.openapi_schema
    openapi_schema = get_openapi(
        title=app.title,
        version=app.version,
        description=app.description,
        routes=app.routes,
        contact=app.contact,
        license_info=app.license_info,
        tags=app.openapi_tags,
    )
    openapi_schema["info"]["x-logo"] = {"url": "https://raw.githubusercontent.com/Liamnunez/supervisor-bombas-arduino-uno-q/main/docs/logo.png"}
    app.openapi_schema = openapi_schema
    return app.openapi_schema

app.openapi = custom_openapi

# Servir archivos estaticos
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


@app.post("/api/auth/login", tags=["auth"])
async def login(req: LoginRequest):
    """Intercambia contrasena por token (operator o admin)."""
    result = auth_manager.login(req.password)
    if not result:
        auth_manager.audit("login", None, False, "credenciales invalidas")
        raise HTTPException(401, "Credenciales invalidas")
    auth_manager.audit("login", result["role"], True)
    return result


async def heartbeat_task():
    """Enviar heartbeat periodico a WebSockets y solicitar estado al MCU"""
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


@app.get("/api/status", tags=["telemetria"])
async def get_status():
    return state_manager.status.to_dict()


@app.get("/api/stats", tags=["telemetria"])
async def get_stats():
    return state_manager.get_stats()


@app.get("/api/alerts", tags=["telemetria"])
async def get_alerts(limit: int = 100):
    return [a.to_dict() for a in alert_manager.get_recent(limit)]


# ============================================================
# Nuevos endpoints: historial, exportacion, umbrales, bot
# ============================================================

class ThresholdsConfig(BaseModel):
    # AlertManager thresholds
    nivel_critico_bajo: Optional[int] = None
    nivel_critico_alto: Optional[int] = None
    feedback_timeout_s: Optional[int] = None
    heartbeat_timeout_s: Optional[int] = None
    dedup_window_s: Optional[int] = None
    # CurrentProtector thresholds (se envian al MCU si se implementa)
    corriente_aviso_a: Optional[float] = None
    corriente_trip_a: Optional[float] = None
    corriente_reset_a: Optional[float] = None


@app.get("/api/history", tags=["telemetria"])
async def get_history(
    hours: int = 24,
    limit: int = 1000,
    request: Request = None
):
    """Historial de alertas para graficas (ultimas N horas)."""
    if request:
        role = auth_manager.verify(extract_token(request))
        if role is None:
            raise HTTPException(401, "Autenticacion requerida")
    
    from datetime import timedelta
    since = datetime.now() - timedelta(hours=hours)
    alerts = alert_manager.get_recent(limit)
    filtered = [a for a in alerts if a.timestamp >= since]
    return [a.to_dict() for a in filtered]


@app.get("/api/export/csv", tags=["telemetria"])
async def export_csv(hours: int = 24, request: Request = None):
    """Exportar alertas a CSV."""
    if request:
        role = auth_manager.verify(extract_token(request))
        if role is None:
            raise HTTPException(401, "Autenticacion requerida")
    
    from datetime import timedelta
    since = datetime.now() - timedelta(hours=hours)
    alerts = [a for a in alert_manager.get_recent(5000) if a.timestamp >= since]
    
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(["timestamp", "level", "source", "message", "bomba_id", "codigo"])
    for a in alerts:
        writer.writerow([
            a.timestamp.isoformat(),
            a.level,
            a.source,
            a.message,
            a.bomba_id if a.bomba_id is not None else "",
            f"0x{a.codigo:04X}" if a.codigo is not None else ""
        ])
    
    from fastapi.responses import StreamingResponse
    return StreamingResponse(
        io.BytesIO(output.getvalue().encode("utf-8")),
        media_type="text/csv",
        headers={"Content-Disposition": f"attachment; filename=alertas_{datetime.now():%Y%m%d_%H%M}.csv"}
    )


@app.get("/api/config/thresholds", tags=["configuracion"])
async def get_thresholds(request: Request):
    """Obtener umbrales actuales (solo admin/operator)."""
    role = auth_manager.verify(extract_token(request))
    if role is None:
        raise HTTPException(401, "Autenticacion requerida")
    
    return {
        "nivel_critico_bajo": alert_manager.nivel_critico_bajo,
        "nivel_critico_alto": alert_manager.nivel_critico_alto,
        "feedback_timeout_s": alert_manager.feedback_timeout_s,
        "heartbeat_timeout_s": alert_manager.heartbeat_timeout_s,
        "dedup_window_s": alert_manager.dedup_window_s,
        # CurrentProtector thresholds (si se anaden al AlertManager en futuro)
    }


@app.post("/api/config/thresholds", tags=["configuracion"])
async def set_thresholds(cfg: ThresholdsConfig, request: Request):
    """Actualizar umbrales (solo admin)."""
    role = auth_manager.verify(extract_token(request))
    if role != "admin":
        raise HTTPException(403, "Solo admin puede cambiar umbrales")
    
    changed = []
    if cfg.nivel_critico_bajo is not None:
        alert_manager.nivel_critico_bajo = max(0, min(100, cfg.nivel_critico_bajo))
        changed.append("nivel_critico_bajo")
    if cfg.nivel_critico_alto is not None:
        alert_manager.nivel_critico_alto = max(0, min(100, cfg.nivel_critico_alto))
        changed.append("nivel_critico_alto")
    if cfg.feedback_timeout_s is not None:
        alert_manager.feedback_timeout_s = max(1, cfg.feedback_timeout_s)
        changed.append("feedback_timeout_s")
    if cfg.heartbeat_timeout_s is not None:
        alert_manager.heartbeat_timeout_s = max(1, cfg.heartbeat_timeout_s)
        changed.append("heartbeat_timeout_s")
    if cfg.dedup_window_s is not None:
        alert_manager.dedup_window_s = max(0, cfg.dedup_window_s)
        changed.append("dedup_window_s")
    
    auth_manager.audit("set_thresholds", role, True, f"cambiados: {', '.join(changed)}")
    return {"success": True, "changed": changed, "current": await get_thresholds(request)}


@app.get("/api/bot/stats", tags=["bot"])
async def get_bot_stats(request: Request):
    """Telegram Bot stats."""
    role = auth_manager.verify(extract_token(request))
    if role is None:
        raise HTTPException(401, "Autenticacion requerida")
    
    if not telegram_bot:
        return {"configured": False}
    return {"configured": True, **telegram_bot.get_stats()}


@app.post("/api/command", tags=["comandos"])
async def send_command(req: CommandRequest, request: Request):
    # 1. Autenticacion (fail-closed: sin token valido, nada se ejecuta)
    role = auth_manager.verify(extract_token(request))
    if role is None:
        auth_manager.audit(req.command, None, False, "sin token valido")
        raise HTTPException(401, "Autenticacion requerida")

    # 2. Comando conocido
    if req.command not in COMMAND_REQUIRED_ROLE:
        auth_manager.audit(req.command, role, False, "comando desconocido")
        raise HTTPException(400, f"Comando desconocido: {req.command}")

    # 3. Autorizacion por rol
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
        message = f"Emergencia activada (codigo: {codigo:04X})"
        
    elif req.command == "reset_emergencia":
        success = state_manager.reset_emergencia()
        message = "Emergencia reseteada"
        
    elif req.command == "request_status":
        success = state_manager.request_status()
        message = "Estado solicitado"

    # 4. Audit de la ejecucion
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
                    # Permitir comandos via WS tambien
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

    # Autenticacion por WebSocket (mismo esquema que HTTP, fail-closed)
    role = auth_manager.verify(token)
    if role is None:
        auth_manager.audit(str(cmd), None, False, "WS sin token valido")
        await ws.send_text(json.dumps({
            "type": "error", "command": cmd,
            "error": "autenticacion requerida"
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
