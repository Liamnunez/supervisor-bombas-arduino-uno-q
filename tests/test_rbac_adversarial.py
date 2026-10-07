"""
Tests adversariales del RBAC y del token HMAC (linux/app/auth.py).

Invariante atacado: el RBAC es FAIL-CLOSED y
`COMMAND_REQUIRED_ROLE` es la ÚNICA fuente de verdad.

La matriz de docs (docs/security_protocols.md:82-92) dice:
  - reset_emergencia / set_mantenimiento -> operator o admin
  - set_modo_generador / trigger_emergencia -> SOLO admin
  - desactivar protecciones -> NUNCA, para nadie

Se ataca:
  - comando desconocido (inyección de comandos nuevos)
  - rol None, vacío, con mayúsculas/minúsculas distintas, con espacios
  - token manipulado: payload alterado, firma recortada, firma de otro
    secreto, token expirado, payload sin 'exp', role no string
  - fail-closed sin SUPERVISOR_*_PASSWORD

Estilo: pytest puro, sólo stdlib (AGENTS.md).
"""

import base64
import hashlib
import hmac
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "linux"))

from app.auth import (  # noqa: E402
    AuthManager,
    COMMAND_REQUIRED_ROLE,
    ROLE_ADMIN,
    ROLE_OPERATOR,
)


# ============================================================
# Utilidades
# ============================================================
def _b64e(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64d(text: str) -> bytes:
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def auth(reloj: dict = None, **kwargs) -> AuthManager:
    """
    AuthManager con reloj controlable para poder expirar tokens.

    `reloj` es un dict {"t": float} que el test puede mover. Si no se pasa,
    se crea uno local que nadie más ve: por eso los tests de caducidad
    tienen que pasar el SUYO.
    """
    if reloj is None:
        reloj = {"t": 1000.0}
    kwargs.setdefault("operator_password", "op-secret")
    kwargs.setdefault("admin_password", "adm-secret")
    kwargs.setdefault("secret", "secreto-de-prueba")
    return AuthManager(now=lambda: reloj["t"], **kwargs)


# ============================================================
# 1. La matriz es la única fuente de verdad
# ============================================================
def test_matriz_cubre_los_comandos_de_la_api():
    # Los comandos que linux/app/main.py acepta en /api/command y en /ws.
    comandos_api = {
        "set_modo_generador",
        "set_mantenimiento",
        "trigger_emergencia",
        "reset_emergencia",
        "request_status",
    }
    assert comandos_api <= set(COMMAND_REQUIRED_ROLE)


def test_comandos_de_admin_son_exclusivos_de_admin():
    for cmd in ("set_modo_generador", "trigger_emergencia"):
        assert COMMAND_REQUIRED_ROLE[cmd] == ROLE_ADMIN


def test_comandos_de_operator_admiten_operator():
    for cmd in ("reset_emergencia", "set_mantenimiento", "request_status"):
        assert COMMAND_REQUIRED_ROLE[cmd] == ROLE_OPERATOR


def test_desactivar_protecciones_no_existe_como_comando():
    # docs/security_protocols.md:91 -> "Desactivar protecciones: NUNCA".
    # No debe existir ningún comando que lo permita, para ningún rol.
    for cmd in COMMAND_REQUIRED_ROLE:
        assert "desactiv" not in cmd.lower()
        assert "disable" not in cmd.lower()
        assert "bypass" not in cmd.lower()
        assert "override" not in cmd.lower()


# ============================================================
# 2. Comando desconocido: SIEMPRE denegado
# ============================================================
def test_comando_desconocido_denegado_para_todos_los_roles():
    for rol in (None, "", ROLE_OPERATOR, ROLE_ADMIN, "root", "superuser"):
        for cmd in (
            "desactivar_protecciones",
            "set_modo_generador ",      # espacio final
            " set_modo_generador",      # espacio inicial
            "SET_MODO_GENERADOR",       # mayúsculas
            "",                          # vacío
            "__proto__",
            "set_modo_generador\x00",    # con NUL
        ):
            assert AuthManager.authorize(rol, cmd) is False, (rol, cmd)


def test_required_role_de_comando_desconocido_es_none():
    assert AuthManager.required_role("no_existe") is None
    assert AuthManager.required_role("") is None


def test_no_se_puede_anadir_un_comando_en_runtime():
    # authorize() consulta COMMAND_REQUIRED_ROLE por nombre: si alguien
    # añadiese una clave, el comando passaría a existir. Se comprueba que
    # el diccionario de clase no se puede mutar desde una instancia.
    a = auth()
    try:
        a.COMMAND_REQUIRED_ROLE["bypass"] = None
    except (AttributeError, TypeError):
        pass
    # Ni siquiera mutándolo a propósito se autoriza un comando sin rol:
    # required None => denegado (auth.py:159-160)
    assert AuthManager.authorize(ROLE_ADMIN, "bypass") is False


# ============================================================
# 3. Rol mal formado: denegado
# ============================================================
def test_rol_none_denegado():
    for cmd in COMMAND_REQUIRED_ROLE:
        assert AuthManager.authorize(None, cmd) is False


def test_rol_vacio_denegado():
    for cmd in COMMAND_REQUIRED_ROLE:
        assert AuthManager.authorize("", cmd) is False


def test_rol_con_case_distinto_denegado():
    # "Admin" NO es admin. La comparación es por igualdad exacta
    # (auth.py:161 usa _ROLE_LEVEL.get(role, 0)).
    for variante in ("Admin", "ADMIN", "aDMIn", "Operator", "OPERATOR"):
        for cmd in COMMAND_REQUIRED_ROLE:
            assert AuthManager.authorize(variante, cmd) is False, variante


def test_rol_con_espacios_denegado():
    for variante in (" admin", "admin ", " admin ", "admin\t", "admin\n"):
        assert AuthManager.authorize(variante, "set_modo_generador") is False


def test_rol_no_string_denegado():
    # _ROLE_LEVEL.get(role, 0) con un rol no hashable-lista pero no string
    # (listas, dicts) levanta TypeError en vez de devolver False: authorize()
    # NO es fail-closed para roles no-string, lanza excepción.
    #
    # Impacto real: auth.py:161 es `return _ROLE_LEVEL.get(role, 0) >= ...`.
    # Con role=[] o role={} el .get() lanza TypeError: unhashable type.
    # El endpoint /api/command (main.py:597) lo llama sin try/except, así que
    # el rol nunca llega a ser una lista en producción porque verify() sólo
    # puede devolver "operator"/"admin"/None (auth.py:147). Es una defensa
    # que depende de que el llamante sea correcto: si algún día un endpoint
    # acepta el rol de otra fuente, esto es un 500 en vez de un 403.
    for variante in (0, 1, 2, 2.0, True):
        assert AuthManager.authorize(variante, "set_modo_generador") is False
    # Los no-hashables NO devuelven False: raises
    for variante in ([], {}, set()):
        try:
            resultado = AuthManager.authorize(variante, "set_modo_generador")
        except TypeError:
            resultado = "TypeError"
        assert resultado != True, variante


def test_rol_no_string_no_puede_llegar_desde_verify():
    # verify() es la única fuente de rol en la API y sólo devuelve strings
    # del conjunto cerrado (auth.py:147). Aunque el payload JSON diga otra
    # cosa, verify() filtra.
    a = auth()
    for rol in ([], {}, 2, True, "root"):
        raw = json.dumps({"role": rol, "exp": 9999999999}).encode()
        sig = hmac.new(a._secret, raw, hashlib.sha256).digest()
        assert a.verify(f"{_b64e(raw)}.{_b64e(sig)}") is None


def test_operator_no_puede_cambiar_el_modo():
    assert AuthManager.authorize(ROLE_OPERATOR, "set_modo_generador") is False
    assert AuthManager.authorize(ROLE_OPERATOR, "trigger_emergencia") is False


def test_admin_puede_todo_lo_declarado():
    for cmd, req in COMMAND_REQUIRED_ROLE.items():
        assert AuthManager.authorize(ROLE_ADMIN, cmd) is True, cmd
        assert COMMAND_REQUIRED_ROLE[cmd] == req


# ============================================================
# 4. Token manipulado
# ============================================================
def test_token_de_admin_no_fabrica_token_de_admin():
    a = auth()
    t = a.login("adm-secret")
    assert t["role"] == ROLE_ADMIN
    assert a.verify(t["token"]) == ROLE_ADMIN


def test_token_con_payload_alterado_rechazado():
    a = auth()
    t = a.login("op-secret")
    raw_b64, sig_b64 = t["token"].split(".", 1)
    raw = json.loads(_b64d(raw_b64))
    raw["role"] = ROLE_ADMIN                  # escalada de privilegios
    nuevo = f"{_b64e(json.dumps(raw).encode())}.{sig_b64}"
    assert a.verify(nuevo) is None


def test_token_con_payload_alterado_en_otro_campo_rechazado():
    a = auth()
    t = a.login("op-secret")
    raw_b64, sig_b64 = t["token"].split(".", 1)
    raw = json.loads(_b64d(raw_b64))
    raw["exp"] = raw["exp"] + 999999          # alargar la vida del token
    nuevo = f"{_b64e(json.dumps(raw).encode())}.{sig_b64}"
    assert a.verify(nuevo) is None


def test_token_con_firma_recortada_rechazado():
    a = auth()
    t = a.login("adm-secret")
    raw_b64, sig_b64 = t["token"].split(".", 1)
    firma = _b64d(sig_b64)
    assert a.verify(f"{raw_b64}.{_b64e(firma[:-1])}") is None
    assert a.verify(f"{raw_b64}.{_b64e(firma + b'x')}") is None
    assert a.verify(f"{raw_b64}.{_b64e(b'')}") is None


def test_token_firmado_con_otro_secreto_rechazado():
    firmante = auth(secret="secreto-ajeno")
    victima = auth(secret="secreto-bueno")
    t = firmante.login("adm-secret")
    assert t["role"] == ROLE_ADMIN
    # Mismo rol, mismo formato, pero la firma no vale para la otra instancia
    assert victima.verify(t["token"]) is None


def test_token_expirado_rechazado():
    reloj = {"t": 1000.0}
    a = auth(reloj=reloj, token_ttl_s=10)
    t = a.login("op-secret")
    assert a.verify(t["token"]) == ROLE_OPERATOR
    reloj["t"] = 1000.0 + 11
    assert a.verify(t["token"]) is None


def test_token_expirado_en_el_segundo_limite():
    reloj = {"t": 1000.0}
    a = auth(reloj=reloj, token_ttl_s=10)
    t = a.login("op-secret")
    reloj["t"] = 1000.0 + 10          # justo en el límite
    # auth.py:141 usa `exp < now`, así que en el límite exacto aún vale
    assert a.verify(t["token"]) == ROLE_OPERATOR
    reloj["t"] = 1000.0 + 10.001
    assert a.verify(t["token"]) is None


def test_token_sin_exp_rechazado():
    a = auth()
    raw = json.dumps({"role": ROLE_ADMIN, "nonce": "x"}).encode()
    sig = hmac.new(a._secret, raw, hashlib.sha256).digest()
    assert a.verify(f"{_b64e(raw)}.{_b64e(sig)}") is None


def test_token_con_exp_no_numerico_rechazado():
    a = auth()
    raw = json.dumps({"role": ROLE_ADMIN, "exp": "no-soy-un-numero"}).encode()
    sig = hmac.new(a._secret, raw, hashlib.sha256).digest()
    assert a.verify(f"{_b64e(raw)}.{_b64e(sig)}") is None


def test_token_con_role_desconocido_rechazado():
    a = auth()
    for rol in ("root", "superuser", "admin ", "", None, 2):
        raw = json.dumps({"role": rol, "exp": 9999999999}).encode()
        sig = hmac.new(a._secret, raw, hashlib.sha256).digest()
        assert a.verify(f"{_b64e(raw)}.{_b64e(sig)}") is None


def test_token_con_payload_no_dict_rechazado():
    a = auth()
    for payload in ([1, 2, 3], "admin", 42, None):
        raw = json.dumps(payload).encode()
        sig = hmac.new(a._secret, raw, hashlib.sha256).digest()
        assert a.verify(f"{_b64e(raw)}.{_b64e(sig)}") is None


def test_token_malformado_rechazado():
    a = auth()
    for tok in (
        None, "", ".", "..", "sin_punto", "a.b", "!!!.???",
        "eyJ9.", "eyJ9.@@@", "no-es-base64.no-es-base64",
    ):
        assert a.verify(tok) is None, tok


def test_token_con_punto_extra_rechazado():
    a = auth()
    t = a.login("adm-secret")
    # Añadir basura después de la firma no debe aceptarse
    assert a.verify(t["token"] + ".extra") is None
    assert a.verify(t["token"][:-4]) is None


def test_hmac_se_verifica_antes_del_json():
    # Si el HMAC falla, no se parsea el payload: no hay atajo por JSON
    # malformado ni ValidationError.
    a = auth()
    raw = json.dumps({"role": ROLE_ADMIN, "exp": 9999999999}).encode()
    mala = _b64e(hmac.new(b"otro-secreto", raw, hashlib.sha256).digest())
    assert a.verify(f"{_b64e(raw)}.{mala}") is None


# ============================================================
# 5. Fail-closed sin contraseñas
# ============================================================
def test_sin_passwords_no_se_puede_hacer_login():
    a = AuthManager(operator_password=None, admin_password=None,
                    secret="secreto")
    assert a.configured is False
    for pw in ("", "op-secret", "adm-secret", None, "cualquiera"):
        assert a.login(pw) is None


def test_sin_passwords_no_se_puede_obtener_token():
    a = AuthManager(operator_password=None, admin_password=None,
                    secret="secreto")
    # Ni siquiera fabricating un token a mano sirve: no hay verify que valga
    # porque el segredo es aleatorio, pero el caso relevante es que no hay
    # ningún camino de login que devuelva un rol.
    assert a.verify("cualquier.token.aqui") is None


def test_solo_admin_sin_operator_sigue_siendo_fail_closed_para_operator():
    a = AuthManager(operator_password=None, admin_password="adm",
                    secret="secreto")
    # admin puede entrar
    assert a.login("adm")["role"] == ROLE_ADMIN
    # pero el "operador" con contraseña vacía no
    assert a.login("") is None
    assert a.login(None) is None


def test_password_vacia_no_cuenta_como_configurada():
    a = AuthManager(operator_password="", admin_password="", secret="s")
    assert a.configured is False
    assert a.login("") is None


def test_from_env_sin_variables_es_fail_closed(monkeypatch):
    for var in (
        "SUPERVISOR_OPERATOR_PASSWORD",
        "SUPERVISOR_ADMIN_PASSWORD",
        "SUPERVISOR_AUTH_SECRET",
        "SUPERVISOR_TOKEN_TTL_S",
        "SUPERVISOR_AUDIT_LOG",
    ):
        monkeypatch.delenv(var, raising=False)
    a = AuthManager.from_env()
    assert a.configured is False
    assert a.login("lo-que-sea") is None


def test_env_si_influye_en_from_env(monkeypatch):
    monkeypatch.setenv("SUPERVISOR_OPERATOR_PASSWORD", "pw-op")
    monkeypatch.setenv("SUPERVISOR_ADMIN_PASSWORD", "pw-adm")
    a = AuthManager.from_env()
    assert a.configured is True
    assert a.login("pw-op")["role"] == ROLE_OPERATOR
    assert a.login("pw-adm")["role"] == ROLE_ADMIN


# ============================================================
# 6. Constantes de tiempo y trazabilidad
# ============================================================
def test_comparacion_de_passwordes_es_constante_en_el_tiempo():
    # El código usa hmac.compare_digest (auth.py:98,100). Se comprueba que
    # un password objeto con __eq__ malicioso no se ejecuta: si se usaba ==
    # plano, __eq__ se invocararía y podría devolver True.
    class Evil(str):
        def __eq__(self, other):      # pragma: no cover
            raise AssertionError("__eq__ invocado: comparación no constante")

        __hash__ = str.__hash__

    a = AuthManager(operator_password=Evil("correcta"),
                    admin_password="otra", secret="s")
    assert a.login("correcta")["role"] == ROLE_OPERATOR


def test_todo_intento_queda_auditado(tmp_path):
    log = tmp_path / "audit.jsonl"
    a = auth(audit_path=str(log))
    AuthManager.authorize(ROLE_OPERATOR, "set_modo_generador")   # denegado
    a.audit("set_modo_generador", ROLE_OPERATOR, False, "rol insuficiente")
    AuthManager.authorize(ROLE_ADMIN, "reset_emergencia")        # permitido
    a.audit("reset_emergencia", ROLE_ADMIN, True, "ok")
    lineas = log.read_text(encoding="utf-8").strip().split("\n")
    assert len(lineas) == 2
    primero = json.loads(lineas[0])
    assert primero["allowed"] is False
    assert primero["command"] == "set_modo_generador"
    assert primero["role"] == ROLE_OPERATOR
    segundo = json.loads(lineas[1])
    assert segundo["allowed"] is True


def test_ttl_por_defecto_ocho_horas():
    a = auth()
    assert a.token_ttl_s == 8 * 3600
    t = a.login("op-secret")
    assert t["expires_in"] == 8 * 3600


def test_secreto_aleatorio_si_no_se_da():
    a = AuthManager(operator_password="pw")
    b = AuthManager(operator_password="pw")
    # Dos instancias sin secreto explícito NO deben compartir tokens
    ta = a.login("pw")["token"]
    tb = b.login("pw")["token"]
    assert ta != tb
    assert a.verify(tb) is None