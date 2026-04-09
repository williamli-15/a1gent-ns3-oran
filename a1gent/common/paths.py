from __future__ import annotations

import os
from pathlib import Path
from typing import Optional, Union

PathLike = Union[str, os.PathLike]

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WORKSPACE_DIR = REPO_ROOT / "workspace"
DEFAULT_NS3_REF = os.getenv("A1GENT_NS3_REF", "ns-3.42")


def repo_root() -> Path:
    return REPO_ROOT


def _normalize(path: Optional[PathLike]) -> Optional[Path]:
    if path in (None, ""):
        return None
    return Path(path).expanduser().resolve()


def default_ns3_dir() -> Path:
    env_path = _normalize(os.getenv("A1GENT_NS3_DIR"))
    if env_path is not None:
        return env_path
    return (DEFAULT_WORKSPACE_DIR / DEFAULT_NS3_REF).resolve()


def resolve_ns3_dir(ns3_dir: Optional[PathLike] = None) -> Path:
    return _normalize(ns3_dir) or default_ns3_dir()


def resolve_db_path(ns3_dir: Optional[PathLike] = None, db_path: Optional[PathLike] = None) -> Path:
    override = _normalize(db_path) or _normalize(os.getenv("A1GENT_DB_PATH"))
    if override is not None:
        return override
    return resolve_ns3_dir(ns3_dir) / "oran-repository.db"


def resolve_commands_path(
    ns3_dir: Optional[PathLike] = None, commands_path: Optional[PathLike] = None
) -> Path:
    override = _normalize(commands_path) or _normalize(os.getenv("A1GENT_COMMANDS_PATH"))
    if override is not None:
        return override
    return resolve_ns3_dir(ns3_dir) / "commands.json"
