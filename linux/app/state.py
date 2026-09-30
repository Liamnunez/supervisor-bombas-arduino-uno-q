"""
Manejo de estado y recepción de eventos desde MCU vía serial
"""

import serial
import threading
import time
import struct
from typing import Optional, Callable
from datetime import datetime
from .models import SystemStatus, BombaStatus, SystemState, McuEvent, Alert


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
        self.MSG_SIZE = 12  # McuMessage size
        
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
            
            # Solicitar estado inicial
            self.send_command(0x10, 0, 0)  # REQUEST_STATUS
            return True
        except Exception as e:
            print(f"[STATE] Error abriendo puerto {self.port}: {e}")
            return False
    
    def stop(self):
        self.running = False
        if self.rx_thread:
            self.rx_thread.join(timeout=2)
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
        
        # Parsear mensaje (struct: BBBIHB B = 12 bytes)
        # start_byte, msg_type, bomba_id, timestamp, payload, crc8, end_byte
        start_byte, msg_type, bomba_id, timestamp, payload, crc8, end_byte = struct.unpack('<BBBIHBB', data)
        
        if start_byte != 0xAA or end_byte != 0x55:
            return
        
        self.rx_count += 1
        self.last_rx_time = datetime.now()
        
        event = McuEvent(msg_type)
        self._process_event(event, bomba_id, timestamp, payload)
    
    def _validate_crc(self, data: bytes) -> bool:
        # CRC8 sobre bytes 1-10 (excluye start_byte, crc8, end_byte)
        crc = 0
        for b in data[1:11]:
            crc ^= b
            for _ in range(8):
                crc = (crc << 1) ^ 0x07 if (crc & 0x80) else (crc << 1)
        return crc == data[11]
    
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
            
        elif event == McuEvent.HEARTBEAT:
            self.status.uptime_ms = timestamp
            self.status.state = SystemState((payload >> 8) & 0xFF)
            self.status.nivel_agua_pct = payload & 0xFF
            self.status.last_heartbeat = datetime.now()
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
    
    def send_command(self, cmd: int, bomba_id: int, payload: int) -> bool:
        if not self.serial or not self.serial.is_open:
            return False
        
        # Formato: start(0xAA), cmd, bomba_id, timestamp(0), payload, crc8, end(0x55)
        msg = bytearray(12)
        msg[0] = 0xAA
        msg[1] = cmd
        msg[2] = bomba_id & 0xFF
        msg[3:7] = struct.pack('<I', 0)  # timestamp = 0 para comandos
        msg[7:9] = struct.pack('<H', payload)
        msg[10] = 0  # CRC placeholder
        msg[11] = 0x55
        
        # Calcular CRC
        crc = 0
        for b in msg[1:11]:
            crc ^= b
            for _ in range(8):
                crc = (crc << 1) ^ 0x07 if (crc & 0x80) else (crc << 1)
        msg[10] = crc
        
        try:
            self.serial.write(msg)
            self.serial.flush()
            self.tx_count += 1
            return True
        except Exception as e:
            print(f"[STATE] Error enviando comando: {e}")
            return False
    
    # Comandos de alto nivel
    def set_modo_generador(self, generador: bool) -> bool:
        return self.send_command(0x01, 0xFF, 1 if generador else 0)
    
    def set_mantenimiento(self, activo: bool) -> bool:
        return self.send_command(0x02, 0xFF, 1 if activo else 0)
    
    def trigger_emergencia(self, codigo: int) -> bool:
        return self.send_command(0x03, 0xFF, codigo)
    
    def reset_emergencia(self) -> bool:
        return self.send_command(0x04, 0xFF, 0xFFFF)
    
    def request_status(self) -> bool:
        return self.send_command(0x10, 0xFF, 0)
    
    def get_stats(self) -> dict:
        return {
            "rx_count": self.rx_count,
            "tx_count": self.tx_count,
            "crc_errors": self.crc_errors,
            "last_rx": self.last_rx_time.isoformat() if self.last_rx_time else None,
            "connected": self.serial.is_open if self.serial else False
        }