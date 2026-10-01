"""
Autenticación y control de acceso - Supervisor de Bombas

Roles:
  - operator:  reset_emergencia, set_mantenimiento, request_status
               (lo que normalmente necesita el personal en campo)
  - admin:     todo lo anterior + set_modo_generador, trigger_emergencia
               (solo con contraseña de administrador)

Principios:
  - FAIL-CLOSED: si no hay contraseñas configuradas, NINGÚN comando se acepta.
  - El límite de 1 bomba en GENERADOR NO es un comando: es regla fija de
    firmware y nunca se puede sobreescribir por API, ni siquiera con admin.
  - Todo intento (exitoso o no) queda en el audit log.

Sin dependencias externas: solo stdlib (testable con pytest puro).
"""

import hashlib
import hmac
import json
import os
import secrets
import threading
import time
from base64 import urlsafe_b64decode, urlsafe_b64encode
from collections import deque
from pathlib import Path
from typing import Optional

ROLE_OPERATOR = "operator"
ROLE_ADMIN = "admin"

_ROLE_LEVEL = {None: 0, ROLE_OPERATOR: 1, ROLE_ADMIN: 2}

# Nivel mínimo requerido por comando
COMMAND_REQUIRED_ROLE = {
    "request_status": ROLE_OPERATOR,
    "reset_emergencia": ROLE_OPERATOR,
    "set_mantenimiento": ROLE_OPERATOR,
    "set_modo_generador": ROLE_ADMIN,
    "trigger_emergencia": ROLE_ADMIN,
}


def _b64e(data: bytes) -> str:
    return urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64d(text: str) -> bytes:
    return urlsafe_b64decode(text + "=" * (-len(text) % 4))


class AuthManager:
    def __init__(
        self,
        operator_password: Optional[str] = None,
        admin_password: Optional[str] = None,
        secret: Optional[str] = None,
        token_ttl_s: int = 8 * 3600,
        audit_path: Optional[str] = None,
        now=time.time,
    ):
        self._operator_password = operator_password or None
        self._admin_password = admin_password or None
        self._secret = (secret.encode("utf-8") if secret else secrets.token_bytes(32))
        self.token_ttl_s = token_ttl_s
        self._now = now
        self._lock = threading.Lock()
        self.audit_path = Path(audit_path) if audit_path else None
        # Historial en memoria (para tests y consulta rápida)
        self.recent_entries: deque = deque(maxlen=500)

    @classmethod
    def from_env(cls) -> "AuthManager":
        return cls(
            operator_password=os.environ.get("SUPERVISOR_OPERATOR_PASSWORD"),
            admin_password=os.environ.get("SUPERVISOR_ADMIN_PASSWORD"),
            secret=os.environ.get("SUPERVISOR_AUTH_SECRET"),
            token_ttl_s=int(os.environ.get("SUPERVISOR_TOKEN_TTL_S", 8 * 3600)),
            audit_path=os.environ.get("SUPERVISOR_AUDIT_LOG"),
        )

    @property
    def configured(self) -> bool:
        """Hay al menos una contraseña definida? (si no: fail-closed)"""
        return bool(self._operator_password or self._admin_password)

    # ---------------------------------------------------------------
    # Login / tokens
    # ---------------------------------------------------------------
    def login(self, password: Optional[str]) -> Optional[dict]:
        """Devuelve {"token", "role", "expires_in"} o None si es inválido."""
        if not self.configured or not password:
            return None

        role = None
        if self._admin_password and hmac.compare_digest(password, self._admin_password):
            role = ROLE_ADMIN
        elif self._operator_password and hmac.compare_digest(password, self._operator_password):
            role = ROLE_OPERATOR

        if role is None:
            return None

        token = self._issue(role)
        return {"token": token, "role": role, "expires_in": self.token_ttl_s}

    def _issue(self, role: str) -> str:
        payload = {
            "role": role,
            "exp": int(self._now()) + self.token_ttl_s,
            "nonce": secrets.token_hex(8),
        }
        raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        sig = hmac.new(self._secret, raw, hashlib.sha256).digest()
        return f"{_b64e(raw)}.{_b64e(sig)}"

    def verify(self, token: Optional[str]) -> Optional[str]:
        """Devuelve el role si el token es válido y no expiró, si no None."""
        if not token or "." not in token:
            return None
        try:
            raw_b64, sig_b64 = token.split(".", 1)
            raw = _b64d(raw_b64)
            sig = _b64d(sig_b64)
        except Exception:
            return None

        expected = hmac.new(self._secret, raw, hashlib.sha256).digest()
        if not hmac.compare_digest(sig, expected):
            return None

        try:
            payload = json.loads(raw)
        except Exception:
            return None
        if not isinstance(payload, dict):
            return None
        try:
            if int(payload.get("exp", 0)) < self._now():
                return None
        except (TypeError, ValueError):
            return None

        role = payload.get("role")
        return role if role in (ROLE_OPERATOR, ROLE_ADMIN) else None

    # ---------------------------------------------------------------
    # Autorización
    # ---------------------------------------------------------------
    @staticmethod
    def required_role(command: str) -> Optional[str]:
        return COMMAND_REQUIRED_ROLE.get(command)

    @staticmethod
    def authorize(role: Optional[str], command: str) -> bool:
        required = COMMAND_REQUIRED_ROLE.get(command)
        if required is None:
            return False  # Comando desconocido: siempre denegado
        return _ROLE_LEVEL.get(role, 0) >= _ROLE_LEVEL[required]

    # ---------------------------------------------------------------
    # Audit log (JSONL - intentos permitidos Y denegados)
    # ---------------------------------------------------------------
    def audit(self, command: str, role: Optional[str], allowed: bool, detail: str = "") -> dict:
        entry = {
            "ts": time.time(),
            "command": command,
            "role": role,
            "allowed": bool(allowed),
            "detail": detail,
        }
        with self._lock:
            self.recent_entries.append(entry)
            if self.audit_path:
                try:
                    self.audit_path.parent.mkdir(parents=True, exist_ok=True)
                    with open(self.audit_path, "a", encoding="utf-8") as fh:
                        fh.write(json.dumps(entry, separators=(",", ":")) + "\n")
                except OSError:
                    # El audit en disco no debe tumbar la operación
                    pass
        return entry
