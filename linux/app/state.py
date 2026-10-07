"""
Manejo de estado y recepción de eventos desde MCU vía serial
"""

import serial
import threading
import time
import struct
import os
from typing import Optional, Callable
from datetime import datetime
from .models import SystemStatus, BombaStatus, SystemState, McuEvent, Alert

# Mensajes para códigos de error del firmware (0x60xx = corriente/protección)
def _crc8(data: bytes) -> int:
    """CRC8 polinomio 0x07 (Dallas/Maxim) sobre `data`.

    Traducción literal de crc8() en mcu/include/config.h. La máscara & 0xFF
    en C la hacía el propio tipo uint8_t al asignar; en Python los enteros no
    se truncan, así que el CRC crecía hasta ~2^80 y `msg[9] = crc` lanzaba
    ValueError. Por eso el enmascarado es explícito aquí: es la diferencia
    entre "los enteros de Python no tienen anchura fija" y una comunicación
    rota en silencio.

    ⚠️ El rango de bytes que se pasa aquí lo decide el llamante. El
    contrato del protocolo es [1..8]; ver _validate_crc().
    """
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            # En C: crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1)
            # con crc declarado uint8_t, así que cada desplazamiento
            # truncaba a 8 bits.
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


MCU_ERROR_MESSAGES = {
    0x6001: "SOBRECARGA GENERADOR - bombas desconectadas (el sistema re-intenta solo)",
    0x6002: "Corriente con relés abiertos - posible contactor pegado",
    0x6003: "Sensor de corriente (CT) fuera de rango o desconectado",
    0x6010: "Aviso: corriente cerca del límite del generador",
    0x6020: "Trip re-armado automáticamente por el supervisor",
    0x6021: "Trip: intentos agotados - REQUIERE RESET DE OPERADOR",
}

# Nivel de alerta por código. Un re-arm automático es informativo, no crítico:
# si se marcara como crítico, cada tormenta en generador llenaría el canal de
# Telegram de críticos y el equipo dejaría de mirarlo.
MCU_ERROR_LEVELS = {
    0x6010: "warning",
    0x6020: "info",
}


class StateManager:
    def __init__(self, port: str = "/dev/ttyACM0", baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.serial: Optional[serial.Serial] = None
        self.running = False
        self.rx_thread: Optional[threading.Thread] = None
        
        # Estado del sistema
        self.status = SystemStatus()
        
        # Callbacks para notificar cambios
        self.on_status_change: Optional[Callable[[SystemStatus], None]] = None
        self.on_alert: Optional[Callable[[Alert], None]] = None
        self.on_raw_message: Optional[Callable[[bytes], None]] = None
        
        # Buffer RX
        self.rx_buffer = bytearray()
        self.MSG_SIZE = 11  # McuMessage size (11 bytes packed)
        
        # Stats
        self.rx_count = 0
        self.tx_count = 0
        self.crc_errors = 0
        self.last_rx_time: Optional[datetime] = None
    
    def start(self) -> bool:
        try:
            self.serial = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=0.1,  # Non-blocking read
                write_timeout=1
            )
            time.sleep(0.5)  # Wait for Arduino reset
            self.serial.reset_input_buffer()
            
            self.running = True
            self.rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
            self.rx_thread.start()
            
            # Inicializar running_seconds desde BD
            self._init_running_seconds()
            
            # Iniciar tarea de running_seconds
            self._start_running_seconds_task()
            
            # Solicitar estado inicial
            self.send_command(0x10, 0, 0)  # REQUEST_STATUS
            return True
        except Exception as e:
            print(f"[STATE] Error abriendo puerto {self.port}: {e}")
            return False
    
    def stop(self):
        self.running = False
        # Persistir running_seconds finales
        import asyncio
        try:
            for i in range(3):
                asyncio.run(self._persist_running_seconds(i))
        except Exception:
            pass
        if self.rx_thread:
            self.rx_thread.join(timeout=2)
        if hasattr(self, '_running_seconds_task'):
            self._running_seconds_task.cancel()
        if self.serial and self.serial.is_open:
            self.serial.close()
    
    def _rx_loop(self):
        while self.running and self.serial and self.serial.is_open:
            try:
                data = self.serial.read(64)
                if data:
                    self.rx_buffer.extend(data)
                    self._process_buffer()
            except serial.SerialException as e:
                print(f"[STATE] Error serial: {e}")
                time.sleep(1)
            except Exception as e:
                print(f"[STATE] Error inesperado: {e}")
                time.sleep(0.1)
    
    def _process_buffer(self):
        while len(self.rx_buffer) >= self.MSG_SIZE:
            # Buscar start byte 0xAA
            start_idx = -1
            for i in range(len(self.rx_buffer) - self.MSG_SIZE + 1):
                if self.rx_buffer[i] == 0xAA:
                    start_idx = i
                    break
            
            if start_idx == -1:
                # No hay start byte, limpiar buffer
                self.rx_buffer.clear()
                return
            
            if start_idx > 0:
                # Descartar basura antes del start byte
                self.rx_buffer = self.rx_buffer[start_idx:]
            
            if len(self.rx_buffer) < self.MSG_SIZE:
                return  # Mensaje incompleto
            
            # Extraer mensaje
            msg_bytes = self.rx_buffer[:self.MSG_SIZE]
            self.rx_buffer = self.rx_buffer[self.MSG_SIZE:]
            
            self._handle_message(msg_bytes)
    
    def _handle_message(self, data: bytes):
        if self.on_raw_message:
            self.on_raw_message(data)
        
        # Validar CRC
        if not self._validate_crc(data):
            self.crc_errors += 1
            return
        
        # Parsear mensaje (struct: BBBIHBB = 11 bytes)
        # start_byte, msg_type, bomba_id, timestamp, payload, crc8, end_byte
        start_byte, msg_type, bomba_id, timestamp, payload, crc8, end_byte = struct.unpack('<BBBIHBB', data)
        
        if start_byte != 0xAA or end_byte != 0x55:
            return
        
        self.rx_count += 1
        self.last_rx_time = datetime.now()
        
        event = McuEvent(msg_type)
        self._process_event(event, bomba_id, timestamp, payload)
    
    def _validate_crc(self, data: bytes) -> bool:
        """CRC8 sobre los offsets [1..8] (8 bytes).

        Formato: [0]=0xAA, [1]=msg_type, [2]=bomba_id, [3-6]=timestamp,
        [7-8]=payload, [9]=crc8, [10]=0x55.

        Se excluyen los dos bytes de encuadre (0 y 10) y el propio CRC (9),
        que no puede firmarse a sí mismo.

        ⚠️ Rango triplicado: debe coincidir byte a byte con
        validar_mensaje() en mcu/include/config.h y buildMessage() en
        mcu/src/comm_bridge.cpp. Las tres copias discrepaban y el firmware
        no aceptaba sus propios frames.
        """
        crc = _crc8(data[1:9])
        return crc == data[9]
    
    def _process_event(self, event: McuEvent, bomba_id: int, timestamp: int, payload: int):
        changed = False
        
        if event == McuEvent.STATE_CHANGE:
            anterior = SystemState((payload >> 8) & 0xFF)
            nuevo = SystemState(payload & 0xFF)
            self.status.previous_state = anterior
            self.status.state = nuevo
            changed = True
            self._emit_alert("info", "STATE", f"Cambio de estado: {anterior.label} -> {nuevo.label}")
            
        elif event == McuEvent.MODO_CHANGE:
            self.status.modo_generador_hw = bool(payload & 0x01)
            changed = True
            modo = "GENERADOR" if self.status.modo_generador_hw else "RED"
            self._emit_alert("info", "MODO", f"Cambio de modo: {modo}")
            
        elif event == McuEvent.NIVEL_UPDATE:
            self.status.nivel_agua_pct = payload & 0xFF
            changed = True
            
        elif event == McuEvent.CORRIENTE_UPDATE:
            # Corriente total del generador (deciamperios -> A), CT propio
            self.status.corriente_a = payload / 10.0
            changed = True
            
        elif event == McuEvent.ERROR:
            # Error del firmware (p.ej. protección de corriente 0x60xx)
            level = MCU_ERROR_LEVELS.get(payload, "critical")
            self._emit_alert(
                level, "MCU",
                MCU_ERROR_MESSAGES.get(payload, f"Error MCU: 0x{payload:04X}"),
                codigo=payload
            )
            changed = True
        elif event == McuEvent.HEARTBEAT:
            self.status.uptime_ms = timestamp
            self.status.state = SystemState((payload >> 8) & 0xFF)
            self.status.nivel_agua_pct = payload & 0x7F
            self.status.sensor_ok = bool(payload & 0x80)
            self.status.last_heartbeat = datetime.now()
            changed = True
            
        elif event == McuEvent.NIVEL_UPDATE:
            self.status.nivel_agua_pct = payload & 0x7F
            self.status.sensor_ok = bool(payload & 0x80)
            changed = True
            
        elif event in (McuEvent.BOMBA_START, McuEvent.BOMBA_STOP, 
                       McuEvent.BOMBA_FAULT, McuEvent.FEEDBACK_MISMATCH):
            if 0 <= bomba_id < 3:
                b = self.status.bombas[bomba_id]
                if event == McuEvent.BOMBA_START:
                    b.feedback = True
                    b.last_change = datetime.now()
                    self._emit_alert("info", f"B{bomba_id+1}", "Bomba arrancada")
                elif event == McuEvent.BOMBA_STOP:
                    b.feedback = False
                    b.last_change = datetime.now()
                    self._emit_alert("info", f"B{bomba_id+1}", "Bomba parada")
                elif event == McuEvent.BOMBA_FAULT:
                    b.fault_count += 1
                    self._emit_alert("critical", f"B{bomba_id+1}", f"FAULT detectado (cuenta: {b.fault_count})", bomba_id, 0x22)
                elif event == McuEvent.FEEDBACK_MISMATCH:
                    self._emit_alert("warning", f"B{bomba_id+1}", "Mismatch PLC vs Feedback", bomba_id, 0x23)
                changed = True
        
        if changed and self.on_status_change:
            self.on_status_change(self.status)
    
    def _emit_alert(self, level: str, source: str, message: str, bomba_id: int = None, codigo: int = None):
        alert = Alert(
            timestamp=datetime.now(),
            level=level,
            source=source,
            message=message,
            bomba_id=bomba_id,
            codigo=codigo
        )
        if self.on_alert:
            self.on_alert(alert)
    
    def send_command(self, event: int, bomba_id: int, payload: int) -> bool:
        """Enviar comando alineado con firmware:
        event = McuEvent opcode (0x40 MODO_CHANGE, 0x10 STATE_CHANGE, 0xFF ERROR)
        bomba_id = 0xFF (N/A para comandos de estado)
        payload = según spec firmware"""
        if not self.serial or not self.serial.is_open:
            return False
        
        msg = bytearray(11)
        msg[0] = 0xAA
        msg[1] = event & 0xFF
        msg[2] = bomba_id & 0xFF
        msg[3:7] = struct.pack('<I', 0)  # timestamp = 0 para comandos
        msg[7:9] = struct.pack('<H', payload)
        # msg[9] = crc placeholder (will be overwritten)
        msg[10] = 0x55
        
        # Calcular CRC8 sobre los offsets [1..8] (mismo rango que
        # validar_mensaje() y buildMessage()). Ver _validate_crc().
        msg[9] = _crc8(msg[1:9])
        
        try:
            self.serial.write(msg)
            self.serial.flush()
            self.tx_count += 1
            return True
        except Exception as e:
            print(f"[STATE] Error enviando comando: {e}")
            return False
    
    # Comandos de alto nivel (mapeados a opcodes de firmware)
    # Firmware espera:
    # - MODO_CHANGE (0x40): payload bit 0 = 1 (GEN) / 0 (RED)
    # - STATE_CHANGE (0x10): payload = (error_code << 8) | state
    # - ERROR (0xFF): payload = 0xFFFF para reset
    def set_modo_generador(self, generador: bool) -> bool:
        return self.send_command(0x40, 0xFF, 1 if generador else 0)
    
    def set_mantenimiento(self, activo: bool) -> bool:
        if activo:
            # STATE_CHANGE con state=MANTENIMIENTO (3)
            return self.send_command(0x10, 0xFF, (0 << 8) | 3)
        else:
            # Reset mantenimiento: forzar modo según HW (ERROR 0xFFFF limpia todo)
            return self.send_command(0xFF, 0xFF, 0xFFFF)
    
    def trigger_emergencia(self, codigo: int) -> bool:
        # STATE_CHANGE con state=EMERGENCIA (2) y error_code en high byte
        return self.send_command(0x10, 0xFF, ((codigo & 0xFF) << 8) | 2)
    
    def reset_emergencia(self) -> bool:
        # ERROR con payload 0xFFFF
        return self.send_command(0xFF, 0xFF, 0xFFFF)
    
    def request_status(self) -> bool:
        # No hay opcode específico para request status en firmware;
        # se puede usar HEARTBEAT request o similar. De momento no-op.
        return True
    
    def get_stats(self) -> dict:
        return {
            "rx_count": self.rx_count,
            "tx_count": self.tx_count,
            "crc_errors": self.crc_errors,
            "last_rx": self.last_rx_time.isoformat() if self.last_rx_time else None,
            "connected": self.serial.is_open if self.serial else False
        }
    
    # ============================================================
    # Running seconds tracking (maintenance)
    # ============================================================
    
    def _init_running_seconds(self):
        """Inicializa running_seconds desde base de datos persistente."""
        try:
            import sqlite3
            db_path = os.environ.get("SUPERVISOR_MAINTENANCE_DB", "maintenance.db")
            conn = sqlite3.connect(db_path, timeout=10)
            conn.execute("""
                CREATE TABLE IF NOT EXISTS pump_runtime (
                    bomba_id INTEGER PRIMARY KEY,
                    running_seconds INTEGER NOT NULL DEFAULT 0,
                    updated_at TEXT NOT NULL
                )
            """)
            conn.commit()
            for i in range(3):
                row = conn.execute(
                    "SELECT running_seconds FROM pump_runtime WHERE bomba_id = ?", (i,)
                ).fetchone()
                if row:
                    self.status.bombas[i].running_seconds = row[0]
                    self.status.bombas[i].last_running_update = datetime.now()
            conn.close()
        except Exception as e:
            print(f"[STATE] No se pudo cargar runtime: {e}")
    
    def _start_running_seconds_task(self):
        """Inicia tarea en background que actualiza running_seconds cada segundo."""
        import asyncio
        self._running_seconds_task = asyncio.create_task(self._running_seconds_loop())
    
    async def _running_seconds_loop(self):
        """Loop que incrementa running_seconds para bombas en marcha cada segundo."""
        while self.running:
            await asyncio.sleep(1)
            now = datetime.now()
            for b in self.status.bombas:
                if b.running:
                    if b.last_running_update is None:
                        b.last_running_update = now
                    else:
                        delta = (now - b.last_running_update).total_seconds()
                        if delta >= 1:
                            b.running_seconds += int(delta)
                            b.last_running_update = now
                            # Persistir cada 60s o en cambios significativos
                            if b.running_seconds % 60 == 0:
                                await self._persist_running_seconds(b.id)
                else:
                    b.last_running_update = None
    
    async def _persist_running_seconds(self, bomba_id: int):
        """Persiste running_seconds a SQLite."""
        try:
            import sqlite3
            import os
            db_path = os.environ.get("SUPERVISOR_MAINTENANCE_DB", "maintenance.db")
            conn = sqlite3.connect(db_path, timeout=5)
            conn.execute(
                "INSERT OR REPLACE INTO pump_runtime (bomba_id, running_seconds, updated_at) VALUES (?, ?, ?)",
                (bomba_id, self.status.bombas[bomba_id].running_seconds, datetime.now().isoformat())
            )
            conn.commit()
            conn.close()
        except Exception:
            pass  # No bloquear por error de persistencia
    
    def get_maintenance_status(self) -> dict:
        """Retorna estado de mantenimiento para cada bomba."""
        # Umbrales por defecto (configurables via env)
        oil_threshold_h = int(os.environ.get("MAINT_OIL_HOURS", "500"))
        bearing_threshold_h = int(os.environ.get("MAINT_BEARING_HOURS", "2000"))
        
        result = {}
        for b in self.status.bombas:
            hours = b.running_hours
            oil_due = hours >= oil_threshold_h
            bearing_due = hours >= bearing_threshold_h
            result[b.id] = {
                "running_hours": round(hours, 2),
                "running_seconds": b.running_seconds,
                "oil_change_due": oil_due,
                "bearing_due": bearing_due,
                "oil_threshold_h": oil_threshold_h,
                "bearing_threshold_h": bearing_threshold_h,
                "hours_until_oil": max(0, oil_threshold_h - hours),
                "hours_until_bearing": max(0, bearing_threshold_h - hours),
            }
        return result