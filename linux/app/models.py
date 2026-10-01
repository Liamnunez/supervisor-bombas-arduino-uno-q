"""
Modelos de datos para telemetría - Supervisor de Bombas
"""

from enum import IntEnum
from dataclasses import dataclass, field
from typing import Optional
from datetime import datetime
import json


class SystemState(IntEnum):
    NORMAL = 0
    GENERADOR = 1
    EMERGENCIA = 2
    MANTENIMIENTO = 3
    
    @property
    def label(self) -> str:
        return {
            SystemState.NORMAL: "NORMAL (RED)",
            SystemState.GENERADOR: "GENERADOR",
            SystemState.EMERGENCIA: "EMERGENCIA",
            SystemState.MANTENIMIENTO: "MANTENIMIENTO"
        }[self]
    
    @property
    def color(self) -> str:
        return {
            SystemState.NORMAL: "#22c55e",
            SystemState.GENERADOR: "#eab308",
            SystemState.EMERGENCIA: "#ef4444",
            SystemState.MANTENIMIENTO: "#6366f1"
        }[self]


class McuEvent(IntEnum):
    STATE_CHANGE = 0x10
    BOMBA_START = 0x20
    BOMBA_STOP = 0x21
    BOMBA_FAULT = 0x22
    FEEDBACK_MISMATCH = 0x23
    NIVEL_UPDATE = 0x30
    CORRIENTE_UPDATE = 0x31
    MODO_CHANGE = 0x40
    HEARTBEAT = 0x50
    ERROR = 0xFF


@dataclass
class BombaStatus:
    id: int  # 0, 1, 2
    plc_order: bool = False
    feedback: bool = False
    relay_closed: bool = False
    fault_count: int = 0
    last_change: Optional[datetime] = None
    running_seconds: int = 0       # Acumulado de segundos en marcha
    last_running_update: Optional[datetime] = None  # Ultima vez que se actualizo running_seconds
    
    @property
    def running(self) -> bool:
        return self.feedback and self.relay_closed
    
    @property
    def mismatch(self) -> bool:
        return self.plc_order != self.feedback
    
    @property
    def running_hours(self) -> float:
        return self.running_seconds / 3600.0


@dataclass
class SystemStatus:
    state: SystemState = SystemState.NORMAL
    previous_state: SystemState = SystemState.NORMAL
    modo_generador_hw: bool = False
    modo_mantenimiento: bool = False
    emergencia_activa: bool = False
    emergencia_codigo: int = 0
    nivel_agua_pct: int = 0
    corriente_a: float = 0.0        # Corriente total del generador (CT propio)
    sensor_ok: bool = True
    uptime_ms: int = 0
    last_heartbeat: Optional[datetime] = None
    bombas: list = field(default_factory=lambda: [BombaStatus(i) for i in range(3)])
    
    def to_dict(self) -> dict:
        return {
            "state": self.state.value,
            "state_label": self.state.label,
            "state_color": self.state.color,
            "previous_state": self.previous_state.value,
            "modo_generador_hw": self.modo_generador_hw,
            "modo_mantenimiento": self.modo_mantenimiento,
            "emergencia_activa": self.emergencia_activa,
            "emergencia_codigo": self.emergencia_codigo,
            "nivel_agua_pct": self.nivel_agua_pct,
            "corriente_a": round(self.corriente_a, 1),
            "sensor_ok": self.sensor_ok,
            "uptime_ms": self.uptime_ms,
            "last_heartbeat": self.last_heartbeat.isoformat() if self.last_heartbeat else None,
            "bombas": [
                {
                    "id": b.id,
                    "plc_order": b.plc_order,
                    "feedback": b.feedback,
                    "relay_closed": b.relay_closed,
                    "fault_count": b.fault_count,
                    "running": b.running,
                    "mismatch": b.mismatch,
                    "last_change": b.last_change.isoformat() if b.last_change else None,
                    "running_seconds": b.running_seconds,
                    "running_hours": round(b.running_hours, 2)
                }
                for b in self.bombas
            ]
        }


@dataclass
class Alert:
    timestamp: datetime
    level: str  # "info", "warning", "critical"
    source: str
    message: str
    bomba_id: Optional[int] = None
    codigo: Optional[int] = None
    alert_id: Optional[int] = None   # id en el almacén persistente (SQLite)
    
    def to_dict(self) -> dict:
        return {
            "id": self.alert_id,
            "timestamp": self.timestamp.isoformat(),
            "level": self.level,
            "source": self.source,
            "message": self.message,
            "bomba_id": self.bomba_id,
            "codigo": self.codigo
        }


# Comandos hacia MCU
class McuCommand(IntEnum):
    SET_MODO_GENERADOR = 0x01
    SET_MANTENIMIENTO = 0x02
    TRIGGER_EMERGENCIA = 0x03
    RESET_EMERGENCIA = 0x04
    REQUEST_STATUS = 0x10