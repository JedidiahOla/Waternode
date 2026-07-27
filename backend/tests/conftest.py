"""
Shared fixtures. Sets secrets and a temp DATABASE_PATH before any project
module is imported, so tests don't need .env and never touch a real DB.
"""

import os
import sys
from pathlib import Path

import tempfile

os.environ.setdefault("SECRET_KEY", "test-secret-key")
os.environ.setdefault("API_KEY", "test-api-key")
os.environ.setdefault("DASHBOARD_REQUIRE_AUTH", "false")

# app.py calls init_db() on import. Point that at a throwaway file so the
# import doesn't leave a stray waternode.db in the repo.
os.environ.setdefault("DATABASE_PATH",
                      os.path.join(tempfile.mkdtemp(), "import_time.db"))

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import pytest


@pytest.fixture(autouse=True)
def isolated_database(tmp_path, monkeypatch):
    """Point every test at its own throwaway SQLite file."""
    import config
    import database as db

    db_path = tmp_path / "test_waternode.db"
    monkeypatch.setattr(config, "DATABASE_PATH", str(db_path))
    db.init_db()
    yield
