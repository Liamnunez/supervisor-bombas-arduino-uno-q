"""
Telegram Bot para alertas - Supervisor de Bombas

- Polling de alertas pendientes desde AlertStore (store-and-forward)
- Envío con reintentos y backoff
- Comandos: /status, /alerts, /help
- Sin dependencias externas (solo stdlib + requests si está disponible)
"""

import json
import os
import threading
import time
from datetime import datetime
from typing import Optional
from urllib.parse import urlencode
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError

from .alerts import AlertManager
from .models import Alert


class TelegramBot:
    def __init__(
        self,
        token: str,
        chat_id: str,
        alert_manager: AlertManager,
        poll_interval_s: int = 10,
        max_retries: int = 3,
        base_timeout_s: float = 10.0,
    ):
        self.token = token
        self.chat_id = chat_id
        self.alert_manager = alert_manager
        self.poll_interval_s = poll_interval_s
        self.max_retries = max_retries
        self.base_timeout_s = base_timeout_s
        self.base_url = f"https://api.telegram.org/bot{token}"

        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._last_update_id = 0
        self._lock = threading.Lock()

        # Estado
        self.last_send_ok: Optional[datetime] = None
        self.last_send_error: Optional[str] = None
        self.sent_count = 0
        self.failed_count = 0

    def start(self) -> bool:
        if self._running:
            return True
        if not self.token or not self.chat_id:
            return False
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return True

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=5)

    def _run(self):
        while self._running:
            try:
                self._poll_commands()
                self._deliver_pending_alerts()
            except Exception as e:
                # No dejar que un error tumbe el hilo
                self.last_send_error = f"loop error: {e}"
            time.sleep(self.poll_interval_s)

    # ---------------------------------------------------------------
    # Envío de mensajes
    # ---------------------------------------------------------------
    def _send_message(self, text: str, parse_mode: str = "HTML") -> bool:
        url = f"{self.base_url}/sendMessage"
        data = urlencode({
            "chat_id": self.chat_id,
            "text": text,
            "parse_mode": parse_mode,
            "disable_web_page_preview": "true",
        }).encode("utf-8")

        for attempt in range(1, self.max_retries + 1):
            try:
                req = Request(url, data=data, headers={"Content-Type": "application/x-www-form-urlencoded"})
                with urlopen(req, timeout=self.base_timeout_s) as resp:
                    if resp.status == 200:
                        with self._lock:
                            self.last_send_ok = datetime.now()
                            self.last_send_error = None
                            self.sent_count += 1
                        return True
            except (URLError, HTTPError, TimeoutError) as e:
                wait = min(2 ** attempt, 30)
                time.sleep(wait)

        with self._lock:
            self.last_send_error = f"failed after {self.max_retries} retries"
            self.failed_count += 1
        return False

    def _format_alert(self, alert: Alert) -> str:
        level_emoji = {"critical": "🔴", "warning": "🟡", "info": "🔵"}.get(alert.level, "⚪")
        ts = alert.timestamp.strftime("%Y-%m-%d %H:%M:%S")
        src = alert.source
        msg = alert.message
        if alert.bomba_id is not None:
            src = f"B{alert.bomba_id + 1}"
        if alert.codigo is not None:
            msg += f" (0x{alert.codigo:04X})"
        return f"{level_emoji} <b>{src}</b> — {msg}\n<code>{ts}</code>"

    # ---------------------------------------------------------------
    # Entrega de alertas pendientes (store-and-forward)
    # ---------------------------------------------------------------
    def _deliver_pending_alerts(self):
        pending = self.alert_manager.pending_delivery(limit=50)
        if not pending:
            return

        for alert in pending:
            text = self._format_alert(alert)
            if self._send_message(text):
                self.alert_manager.mark_delivered([alert.alert_id])
            else:
                # Si falla uno, paramos y reintentamos en el siguiente ciclo
                break

    # ---------------------------------------------------------------
    # Polling de comandos de usuario
    # ---------------------------------------------------------------
    def _poll_commands(self):
        url = f"{self.base_url}/getUpdates"
        params = urlencode({"offset": self._last_update_id + 1, "timeout": 5})
        full_url = f"{url}?{params}"

        try:
            req = Request(full_url)
            with urlopen(req, timeout=10) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except Exception:
            return

        if not data.get("ok"):
            return

        for update in data.get("result", []):
            self._last_update_id = update["update_id"]
            msg = update.get("message") or update.get("edited_message")
            if not msg:
                continue
            chat_id = str(msg.get("chat", {}).get("id"))
            if chat_id != self.chat_id:
                continue  # Ignorar chats no autorizados
            text = msg.get("text", "").strip()
            if not text.startswith("/"):
                continue
            self._handle_command(text)

    def _handle_command(self, text: str):
        parts = text.split()
        cmd = parts[0].lower()

        if cmd == "/status":
            self._cmd_status()
        elif cmd == "/alerts":
            limit = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 10
            self._cmd_alerts(limit)
        elif cmd == "/help":
            self._cmd_help()
        elif cmd == "/ping":
            self._send_message("🏓 pong")
        else:
            self._send_message(f"❓ Comando desconocido: {cmd}\nUsa /help")

    def _cmd_status(self):
        # Usar el estado actual del alert_manager para stats
        stats = self.alert_manager.get_stats()
        # Añadir info del bot
        lines = [
            "📊 <b>Estado del Bot</b>",
            f"Alertas almacenadas: {stats.get('stored_total', 0)}",
            f"Pendientes de envío: {stats.get('pending_delivery', 0)}",
            f"Suprimidas (dedup): {stats.get('suppressed', 0)}",
            f"Enviadas OK: {self.sent_count}",
            f"Fallidas: {self.failed_count}",
            f"Último envío OK: {self.last_send_ok.strftime('%H:%M:%S') if self.last_send_ok else 'nunca'}",
            f"Último error: {self.last_send_error or 'ninguno'}",
        ]
        self._send_message("\n".join(lines))

    def _cmd_alerts(self, limit: int):
        recent = self.alert_manager.get_recent(limit)
        if not recent:
            self._send_message("📭 Sin alertas recientes")
            return
        lines = [f"📋 <b>Últimas {len(recent)} alertas</b>"]
        for a in recent[-limit:]:
            lines.append(self._format_alert(a))
        self._send_message("\n\n".join(lines))

    def _cmd_help(self):
        lines = [
            "🤖 <b>Comandos disponibles</b>",
            "/status — Estado del bot y colas",
            "/alerts [N] — Últimas N alertas (defecto 10)",
            "/ping — Test de conectividad",
            "/help — Esta ayuda",
        ]
        self._send_message("\n".join(lines))

    def get_stats(self) -> dict:
        with self._lock:
            return {
                "running": self._running,
                "sent_count": self.sent_count,
                "failed_count": self.failed_count,
                "last_send_ok": self.last_send_ok.isoformat() if self.last_send_ok else None,
                "last_send_error": self.last_send_error,
                "chat_id": self.chat_id,
            }


def create_bot_from_env(alert_manager: AlertManager) -> Optional[TelegramBot]:
    """Factory: crea bot si TELEGRAM_BOT_TOKEN y TELEGRAM_CHAT_ID están en entorno."""
    token = os.environ.get("TELEGRAM_BOT_TOKEN")
    chat_id = os.environ.get("TELEGRAM_CHAT_ID")
    if not token or not chat_id:
        return None
    return TelegramBot(token, chat_id, alert_manager)