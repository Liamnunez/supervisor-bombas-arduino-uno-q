#!/usr/bin/env python3
"""
Tests para autenticación y control de acceso - Supervisor de Bombas
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'linux'))

from app.auth import (
    AuthManager,
    COMMAND_REQUIRED_ROLE,
    ROLE_ADMIN,
    ROLE_OPERATOR,
)


class FakeClock:
    def __init__(self, t=1_700_000_000.0):
        self.t = t

    def __call__(self):
        return self.t


def make(**kw):
    clock = kw.pop("clock", FakeClock())
    return AuthManager(
        operator_password=kw.get("operator_password", "op-secret"),
        admin_password=kw.get("admin_password", "ad-secret"),
        secret="unit-test-secret",
        now=clock,
    ), clock


def test_login_roles():
    auth, _ = make()
    assert auth.configured

    op = auth.login("op-secret")
    assert op is not None and op["role"] == ROLE_OPERATOR
    assert op["token"] and op["expires_in"] > 0

    ad = auth.login("ad-secret")
    assert ad is not None and ad["role"] == ROLE_ADMIN

    assert auth.login("wrong") is None
    assert auth.login("") is None
    assert auth.login(None) is None


def test_fail_closed_sin_passwords():
    """Sin contraseñas configuradas NINGÚN comando puede autenticarse"""
    auth = AuthManager()  # nada configurado
    assert not auth.configured
    assert auth.login("anything") is None
    # Ningún token podría existir -> authorize con role None lo deniega todo
    for cmd in COMMAND_REQUIRED_ROLE:
        assert AuthManager.authorize(None, cmd) is False


def test_verify_token():
    auth, clock = make()
    token = auth.login("op-secret")["token"]

    assert auth.verify(token) == ROLE_OPERATOR

    # Token adulterado
    tampered = token[:-2] + ("AA" if token[-2:] != "AA" else "BB")
    assert auth.verify(tampered) is None

    # Basura
    assert auth.verify(None) is None
    assert auth.verify("") is None
    assert auth.verify("no-token") is None
    assert auth.verify("a.b") is None

    # Expiración
    clock.t += 9 * 3600  # ttl = 8h
    assert auth.verify(token) is None


def test_verify_token_secreto_distinto():
    """Tokens de otra instancia (secreto distinto) no valen"""
    auth1, _ = make()
    auth2 = AuthManager(
        operator_password="op-secret",
        admin_password="ad-secret",
        secret="other-secret",
    )
    token = auth1.login("op-secret")["token"]
    assert auth2.verify(token) is None


def test_autorizacion_matriz():
    """Matriz rol x comando: operator limitado, admin total, desconocido denegado"""
    cases = [
        # (command, operator, admin)
        ("request_status", True, True),
        ("reset_emergencia", True, True),
        ("set_mantenimiento", True, True),
        ("set_modo_generador", False, True),
        ("trigger_emergencia", False, True),
    ]
    for cmd, op_ok, ad_ok in cases:
        assert AuthManager.authorize(ROLE_OPERATOR, cmd) == op_ok, cmd
        assert AuthManager.authorize(ROLE_ADMIN, cmd) == ad_ok, cmd
        assert AuthManager.authorize(None, cmd) is False, cmd

    # Comando inexistente: siempre denegado (incluso admin)
    assert AuthManager.authorize(ROLE_ADMIN, "volver_el_que_quiera") is False


def test_requisito_por_comando():
    assert AuthManager.required_role("reset_emergencia") == ROLE_OPERATOR
    assert AuthManager.required_role("trigger_emergencia") == ROLE_ADMIN
    assert AuthManager.required_role("no_existe") is None


def test_audit_log_jsonl():
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "audit.log")
        auth, _ = make()
        auth.audit_path = __import__("pathlib").Path(path)

        auth.audit("reset_emergencia", ROLE_OPERATOR, True, "ok")
        auth.audit("trigger_emergencia", ROLE_OPERATOR, False, "rol insuficiente")

        with open(path, encoding="utf-8") as fh:
            lines = [json.loads(l) for l in fh.read().splitlines()]

        assert len(lines) == 2
        assert lines[0]["command"] == "reset_emergencia"
        assert lines[0]["allowed"] is True
        assert lines[0]["role"] == ROLE_OPERATOR
        # Los intentos denegados también quedan registrados
        assert lines[1]["allowed"] is False

        # Historial en memoria
        assert len(auth.recent_entries) == 2


def test_audit_no_lanza_si_falla_escritura():
    auth, _ = make()
    auth.audit_path = __import__("pathlib").Path("/no/existe/dir-imposible/x.log")
    entry = auth.audit("reset_emergencia", ROLE_OPERATOR, True)
    assert entry["allowed"] is True


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"✓ {t.__name__}")
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"✗ {t.__name__}: ERROR - {e}")
            failed += 1
    print("=" * 50)
    print("TODOS LOS TESTS PASARON" if failed == 0 else f"FALLARON {failed}")
    sys.exit(0 if failed == 0 else 1)
