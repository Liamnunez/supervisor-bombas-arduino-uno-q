#!/usr/bin/env python3
"""
Tests para persistencia de alertas (SQLite) y deduplicación
- Sin pérdida de datos entre reinicios (tormentas / caídas de red)
- Cola de entrega pendiente (store-and-forward hacia el personal)
"""

import os
import sys
from datetime import datetime, timedelta

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'linux'))

from app.alerts import AlertManager, AlertStore
from app.models import Alert


def make_alert(ts=None, level="critical", source="MCU", message="overload",
               codigo=0x6001):
    return Alert(
        timestamp=ts or datetime(2026, 1, 1, 12, 0, 0),
        level=level,
        source=source,
        message=message,
        codigo=codigo,
    )


def test_persistencia_entre_instancias():
    """Lo que se guarda se recibe tras 'reiniciar' (nueva instancia, mismo DB)"""
    with tempfile_db() as db:
        m1 = AlertManager(db_path=db)
        m1.add(make_alert())
        m1.add(make_alert(source="B1", message="fault", codigo=0x22))

        # "Reinicio": nueva instancia apuntando al mismo archivo
        m2 = AlertManager(db_path=db)
        recent = m2.get_recent(10)
        assert len(recent) == 2
        assert recent[0].source == "MCU"
        assert recent[1].source == "B1"
        assert all(a.alert_id is not None for a in recent)


def test_dedup_suprime_repetidas():
    """Misma alerta repetida dentro de la ventana -> una sola vez"""
    m = AlertManager(dedup_window_s=60)
    base = datetime(2026, 1, 1, 12, 0, 0)

    assert m.add(make_alert(ts=base)) is True
    # Repetida 5s después -> suprimida
    for i in range(5):
        assert m.add(make_alert(ts=base + timedelta(seconds=i + 1))) is False
    assert len(m.get_recent(10)) == 1
    assert m.suppressed_count == 5

    # Fuera de la ventana (61s después) -> se registra de nuevo
    assert m.add(make_alert(ts=base + timedelta(seconds=61))) is True
    assert len(m.get_recent(10)) == 2


def test_dedup_no_mezcla_alertas_distintas():
    """Alertas con distinto origen/código NO se suprimen entre sí"""
    m = AlertManager(dedup_window_s=60)
    base = datetime(2026, 1, 1, 12, 0, 0)
    assert m.add(make_alert(ts=base, source="B1", codigo=0x22))
    assert m.add(make_alert(ts=base, source="B2", codigo=0x22))
    assert m.add(make_alert(ts=base, source="B1", codigo=0x23))
    assert len(m.get_recent(10)) == 3


def test_cola_entrega_pendiente():
    """Todas las alertas quedan pendientes hasta confirmación de entrega"""
    with tempfile_db() as db:
        m = AlertManager(db_path=db)
        a1, a2, a3 = (make_alert(source=s) for s in ("B1", "B2", "B3"))
        m.add(a1)
        m.add(a2)
        m.add(a3)
        id1, id2, id3 = a1.alert_id, a2.alert_id, a3.alert_id

        pending = m.pending_delivery()
        assert [a.alert_id for a in pending] == [id1, id2, id3]

        # Se confirman 2 entregas -> queda 1 pendiente
        m.mark_delivered([id1, id2])
        pending = m.pending_delivery()
        assert [a.alert_id for a in pending] == [id3]

        # El historial completo sigue intacto
        assert len(m.get_recent(10)) == 3


def test_conteos_y_stats():
    with tempfile_db() as db:
        m = AlertManager(db_path=db, dedup_window_s=60)
        base = datetime(2026, 1, 1, 12, 0, 0)
        m.add(make_alert(ts=base))
        m.add(make_alert(ts=base + timedelta(seconds=1)))  # suprimida

        stats = m.get_stats()
        assert stats["stored_total"] == 1
        assert stats["suppressed"] == 1
        assert stats["pending_delivery"] == 1
        assert stats["by_level"]["critical"] == 1


def test_store_count_since():
    with tempfile_db() as db:
        store = AlertStore(db)
        base = datetime(2026, 1, 1, 12, 0, 0)
        store.add(make_alert(ts=base))
        store.add(make_alert(ts=base + timedelta(minutes=10)))

        assert store.count_since(base + timedelta(minutes=5)) == 1
        assert store.count_since(base) == 2
        assert store.total() == 2


def test_almacen_en_memoria_sin_db():
    """Sin db_path funciona como antes (todo en memoria)"""
    m = AlertManager()
    assert m.store is None
    m.add(make_alert())
    assert len(m.get_recent(10)) == 1
    assert m.pending_delivery() == []
    m.mark_delivered([1])  # no-op, no debe fallar


class tempfile_db:
    """Context manager: ruta de DB temporal"""
    def __enter__(self):
        import tempfile
        self._dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self._dir.name, "alerts.db")
        return self.path

    def __exit__(self, *exc):
        self._dir.cleanup()
        return False


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
