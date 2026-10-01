"""
Gestión de alertas - umbrales, fallos, modo generador

- AlertStore: persistencia SQLite local-first (no se pierden datos si hay
  caídas de red/tormentas) + cola de entrega pendiente (store-and-forward
  para el canal de notificación a personal).
- AlertManager: reglas de umbrales + deduplicación (evita tormentas de
  alertas repetidas) sobre el store.
"""

import sqlite3
import threading
from collections import deque
from datetime import datetime, timedelta
from typing import List, Optional
from .models import Alert, SystemStatus, SystemState


class AlertStore:
    """Almacén persistente de alertas (SQLite). Seguro para acceso desde hilos."""

    def __init__(self, db_path):
        self.db_path = str(db_path)
        self._lock = threading.Lock()
        with self._conn() as conn:
            conn.execute(
                """
                CREATE TABLE IF NOT EXISTS alerts (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    ts TEXT NOT NULL,
                    level TEXT NOT NULL,
                    source TEXT NOT NULL,
                    message TEXT NOT NULL,
                    bomba_id INTEGER,
                    codigo INTEGER,
                    delivered INTEGER NOT NULL DEFAULT 0
                )
                """
            )
            conn.execute(
                "CREATE INDEX IF NOT EXISTS idx_alerts_delivered ON alerts(delivered)"
            )

    def _conn(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.db_path, timeout=10)
        conn.row_factory = sqlite3.Row
        return conn

    @staticmethod
    def _row_to_alert(row) -> Alert:
        return Alert(
            timestamp=datetime.fromisoformat(row["ts"]),
            level=row["level"],
            source=row["source"],
            message=row["message"],
            bomba_id=row["bomba_id"],
            codigo=row["codigo"],
            alert_id=row["id"],
        )

    def add(self, alert: Alert) -> int:
        with self._lock:
            with self._conn() as conn:
                cur = conn.execute(
                    "INSERT INTO alerts (ts, level, source, message, bomba_id, codigo) "
                    "VALUES (?, ?, ?, ?, ?, ?)",
                    (
                        alert.timestamp.isoformat(),
                        alert.level,
                        alert.source,
                        alert.message,
                        alert.bomba_id,
                        alert.codigo,
                    ),
                )
                alert.alert_id = cur.lastrowid
                return cur.lastrowid

    def recent(self, limit: int = 100) -> List[Alert]:
        with self._conn() as conn:
            rows = conn.execute(
                "SELECT * FROM alerts ORDER BY id DESC LIMIT ?", (limit,)
            ).fetchall()
        return [self._row_to_alert(r) for r in reversed(rows)]

    def pending(self, limit: int = 500) -> List[Alert]:
        """Alertas aún NO entregadas al canal de notificación (cola de reenvío)."""
        with self._conn() as conn:
            rows = conn.execute(
                "SELECT * FROM alerts WHERE delivered = 0 ORDER BY id LIMIT ?",
                (limit,),
            ).fetchall()
        return [self._row_to_alert(r) for r in rows]

    def mark_delivered(self, ids: List[int]) -> None:
        if not ids:
            return
        with self._lock:
            with self._conn() as conn:
                conn.executemany(
                    "UPDATE alerts SET delivered = 1 WHERE id = ?",
                    [(i,) for i in ids],
                )

    def count_since(self, since: datetime) -> int:
        with self._conn() as conn:
            row = conn.execute(
                "SELECT COUNT(*) AS n FROM alerts WHERE ts >= ?",
                (since.isoformat(),),
            ).fetchone()
        return int(row["n"])

    def total(self) -> int:
        with self._conn() as conn:
            row = conn.execute("SELECT COUNT(*) AS n FROM alerts").fetchone()
        return int(row["n"])


class AlertManager:
    def __init__(
        self,
        max_alerts: int = 1000,
        db_path: Optional[str] = None,
        dedup_window_s: int = 60,
        clock=datetime.now,
    ):
        self.max_alerts = max_alerts
        self.alerts: deque = deque(maxlen=max_alerts)
        self.store = AlertStore(db_path) if db_path else None
        self.dedup_window_s = dedup_window_s
        self._clock = clock

        # Umbrales configurables
        self.nivel_critico_bajo = 10   # %
        self.nivel_critico_alto = 90   # %
        self.feedback_timeout_s = 5    # segundos
        self.heartbeat_timeout_s = 10  # segundos

        # Estado para detección de cambios
        self.last_nivel_ok = True
        self.last_mcu_connected = True

        # Deduplicación: misma alerta repetida dentro de la ventana -> suprimida
        self._last_stored: dict = {}
        self.suppressed_count = 0

    def _dedup_key(self, alert: Alert):
        return (alert.level, alert.source, alert.codigo, alert.message)

    def add(self, alert: Alert) -> bool:
        """
        Registra una alerta. Devuelve False si fue suprimida por dedup
        (ya se registró una idéntica hace menos de dedup_window_s).
        """
        key = self._dedup_key(alert)
        last = self._last_stored.get(key)
        if last is not None and (alert.timestamp - last) < timedelta(
            seconds=self.dedup_window_s
        ):
            self.suppressed_count += 1
            return False

        self._last_stored[key] = alert.timestamp
        self.alerts.append(alert)
        if self.store:
            self.store.add(alert)
        return True

    def get_recent(self, limit: int = 100) -> List[Alert]:
        if self.store:
            return self.store.recent(limit)
        return list(self.alerts)[-limit:]

    def get_by_level(self, level: str, limit: int = 100) -> List[Alert]:
        return [a for a in list(self.alerts)[-limit:] if a.level == level]

    # ---------------------------------------------------------------
    # Cola de entrega (store-and-forward hacia el canal de personal)
    # ---------------------------------------------------------------
    def pending_delivery(self, limit: int = 500) -> List[Alert]:
        """Alertas pendientes de enviar (para cuando vuelva la conexión)."""
        if self.store:
            return self.store.pending(limit)
        return []

    def mark_delivered(self, alert_ids: List[int]) -> None:
        if self.store:
            self.store.mark_delivered(alert_ids)
    
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
            "suppressed": self.suppressed_count,
            "stored_total": self.store.total() if self.store else len(self.alerts),
            "pending_delivery": len(self.pending_delivery(1000)) if self.store else 0,
            "last_alert": self.alerts[-1].to_dict() if self.alerts else None
        }