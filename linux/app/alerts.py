"""
Gestión de alertas - umbrales,故障, modo generador
"""

from collections import deque
from datetime import datetime, timedelta
from typing import List, Optional
from .models import Alert, SystemStatus, SystemState


class AlertManager:
    def __init__(self, max_alerts: int = 1000):
        self.max_alerts = max_alerts
        self.alerts: deque = deque(maxlen=max_alerts)
        
        # Umbrales configurables
        self.nivel_critico_bajo = 10   # %
        self.nivel_critico_alto = 90   # %
        self.feedback_timeout_s = 5    # segundos
        self.heartbeat_timeout_s = 10  # segundos
        
        # Estado para detección de cambios
        self.last_nivel_ok = True
        self.last_mcu_connected = True
    
    def add(self, alert: Alert):
        self.alerts.append(alert)
    
    def get_recent(self, limit: int = 100) -> List[Alert]:
        return list(self.alerts)[-limit:]
    
    def get_by_level(self, level: str, limit: int = 100) -> List[Alert]:
        return [a for a in list(self.alerts)[-limit:] if a.level == level]
    
    def check_thresholds(self, status: SystemStatus) -> List[Alert]:
        """Verificar umbrales y generar alertas si corresponde"""
        new_alerts = []
        now = datetime.now()
        
        # 1. Nivel de agua crítico
        if status.nivel_agua_pct <= self.nivel_critico_bajo:
            if self.last_nivel_ok:
                new_alerts.append(Alert(
                    timestamp=now,
                    level="critical",
                    source="NIVEL",
                    message=f"Nivel crítico BAJO: {status.nivel_agua_pct}%",
                    codigo=0x30
                ))
            self.last_nivel_ok = False
        elif status.nivel_agua_pct >= self.nivel_critico_alto:
            if self.last_nivel_ok:
                new_alerts.append(Alert(
                    timestamp=now,
                    level="critical",
                    source="NIVEL",
                    message=f"Nivel crítico ALTO: {status.nivel_agua_pct}%",
                    codigo=0x31
                ))
            self.last_nivel_ok = False
        else:
            self.last_nivel_ok = True
        
        # 2. Sensor desconectado/fuera de rango
        if not status.sensor_ok:
            new_alerts.append(Alert(
                timestamp=now,
                level="critical",
                source="SENSOR",
                message="Sensor nivel fuera de rango o desconectado",
                codigo=0x32
            ))
        
        # 3. Modo generador activo
        if status.modo_generador_hw and status.state != SystemState.GENERADOR:
            new_alerts.append(Alert(
                timestamp=now,
                level="warning",
                source="MODO",
                message="Modo GENERADOR detectado pero estado no coincide",
                codigo=0x40
            ))
        
        # 4. Emergencia activa
        if status.emergencia_activa:
            new_alerts.append(Alert(
                timestamp=now,
                level="critical",
                source="EMERGENCIA",
                message=f"EMERGENCIA ACTIVA - Código: {status.emergencia_codigo:04X}",
                codigo=status.emergencia_codigo
            ))
        
        # 5. Fallos en bombas
        for bomba in status.bombas:
            if bomba.fault_count > 0:
                new_alerts.append(Alert(
                    timestamp=now,
                    level="critical",
                    source=f"B{bomba.id+1}",
                    message=f"Bomba con {bomba.fault_count} fallos detectados",
                    bomba_id=bomba.id,
                    codigo=0x22
                ))
            
            if bomba.mismatch:
                new_alerts.append(Alert(
                    timestamp=now,
                    level="warning",
                    source=f"B{bomba.id+1}",
                    message="Mismatch: PLC ordena pero feedback no confirma",
                    bomba_id=bomba.id,
                    codigo=0x23
                ))
        
        # 6. MCU desconectado (sin heartbeat)
        if status.last_heartbeat:
            elapsed = (now - status.last_heartbeat).total_seconds()
            if elapsed > self.heartbeat_timeout_s:
                if self.last_mcu_connected:
                    new_alerts.append(Alert(
                        timestamp=now,
                        level="critical",
                        source="MCU",
                        message=f"Sin heartbeat MCU por {elapsed:.0f}s",
                        codigo=0x50
                    ))
                self.last_mcu_connected = False
            else:
                self.last_mcu_connected = True
        
        return new_alerts
    
    def clear(self):
        self.alerts.clear()
    
    def get_stats(self) -> dict:
        by_level = {}
        for alert in self.alerts:
            by_level[alert.level] = by_level.get(alert.level, 0) + 1
        
        return {
            "total": len(self.alerts),
            "by_level": by_level,
            "last_alert": self.alerts[-1].to_dict() if self.alerts else None
        }