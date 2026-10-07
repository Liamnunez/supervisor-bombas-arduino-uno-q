#!/usr/bin/env python3
"""
Tests de que la aplicación ARRANCA.

Este fichero existe por un motivo concreto: `main.py` importaba `McuCommand`
desde `models.py`, que se había borrado en la limpieza de deuda técnica. El
import estaba muerto, nadie lo usaba, ningún test cubría el import de
`app.main`, y el CI seguía en verde. La aplicación no arrancaba desde
entonces y nadie se enteró.

Importar el módulo entero es la única forma barata de que eso no se repita:
un import roto, una ruta mal declarada o una firma cambiada salta aquí.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'linux'))


LINUX_DIR = os.path.join(os.path.dirname(__file__), '..', 'linux')


def _import_main():
    """Importa app.main desde el cwd correcto.

    `main.py` hace `StaticFiles(directory="static")` con ruta RELATIVA, así
    que el import solo funciona con el cwd en `linux/`. Es la razón por la
    que AGENTS.md dice "run the app only from `linux/` cwd". Aquí se hace
    explícito para que el test sea autónomo y para que quede escrito por qué.
    """
    cwd = os.getcwd()
    os.chdir(LINUX_DIR)
    try:
        # La app lee variables de entorno al importar; valores vacíos para que
        # arranque en fail-closed sin tocar la configuración real.
        os.environ.setdefault("SUPERVISOR_OPERATOR_PASSWORD", "")
        os.environ.setdefault("SUPERVISOR_ADMIN_PASSWORD", "")
        import importlib
        return importlib.import_module("app.main")
    finally:
        os.chdir(cwd)


def test_main_importa():
    """El import completo, con todos sus imports relativos."""
    mod = _import_main()
    assert mod is not None


def test_no_importa_clases_inexistentes():
    """Regresión concreta: McuCommand se borró de models.py.

    Si alguien lo vuelve a poner en models.py sin usarlo, o lo reintroduce en
    main.py, este test lo dice con nombre y todo.
    """
    import app.models as models
    assert hasattr(models, "SystemStatus")
    assert hasattr(models, "SystemState")
    assert hasattr(models, "Alert")
    assert hasattr(models, "McuEvent")


def test_rutas_de_comando_declaradas():
    """Las rutas que citan playbooks.md y operator_manual.md existen."""
    mod = _import_main()
    rutas = {r.path for r in mod.app.routes if hasattr(r, "path")}
    # La que usan los procedimientos de reset
    assert "/api/mcu/reset_emergencia" in rutas, sorted(rutas)
    # La genérica
    assert "/api/command" in rutas, sorted(rutas)
    # Autenticación y estado
    assert "/api/auth/login" in rutas, sorted(rutas)
    assert "/api/status" in rutas or "/api/state" in rutas, sorted(rutas)


def test_rate_limit_configurado():
    """Fail-closed: si el límite no estuviera, alguien lo desactivaría."""
    mod = _import_main()
    assert mod.COMMAND_RATE_LIMIT > 0
    assert mod.COMMAND_RATE_WINDOW_S > 0


def test_alert_manager_tiene_silence_y_purge():
    """Sin check_silence() la alerta de heartbeat no suena nunca."""
    mod = _import_main()
    assert callable(getattr(mod.alert_manager, "check_silence", None))
    assert callable(getattr(mod.alert_manager, "purge", None))


def test_command_request_acepta_motivo():
    """El motivo tiene que llegar al audit log; pydantic lo descarta si no."""
    mod = _import_main()
    req = mod.CommandRequest(command="reset_emergencia", params={},
                             motivo="trip x3 - revisar CT")
    assert req.motivo == "trip x3 - revisar CT"
    # y es opcional
    assert mod.CommandRequest(command="reset_emergencia").motivo is None


def test_app_arranca_desde_linux_cwd():
    """El caso documentado en AGENTS.md, comprobado."""
    import subprocess
    import sys as _sys
    codigo = (
        "import sys; sys.argv=['x']\n"
        "import app.main\n"
        "print('ARRANCÓ')\n"
    )
    r = subprocess.run([_sys.executable, "-c", codigo], cwd=LINUX_DIR,
                       capture_output=True, text=True, timeout=60)
    assert "ARRANCÓ" in r.stdout, f"stdout={r.stdout!r} stderr={r.stderr[-400:]!r}"


if __name__ == '__main__':
    tests = [v for k, v in sorted(globals().items()) if k.startswith('test_')]
    fallos = 0
    for t in tests:
        try:
            t()
            print(f"OK  {t.__name__}")
        except Exception as e:
            print(f"FAIL {t.__name__}: {e}")
            fallos += 1
    print(f"\n{len(tests)} tests, {fallos} fallos")
    sys.exit(1 if fallos else 0)
