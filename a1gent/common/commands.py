import json
import os
import tempfile
import time
from typing import List, Optional
from .models import Action, CommandFile
from .log import setup_logger

log = setup_logger("commands")

def write_commands_json(path: str, actions: List[Action], ts: Optional[float] = None):
    # Build payload
    # If caller passes ts, interpret it as *simulation time seconds*.
    if ts is None:
        ts = time.time()
    out = CommandFile(ts=ts, commands=actions).model_dump()

    # Short summary for logs (do not dump full JSON by default)
    kinds = [a["type"] if isinstance(a, dict) else getattr(a, "type", "?") for a in out["commands"]]
    log.info(f"write -> {path} | actions={len(actions)} | kinds={kinds} | ts={ts:.3f}")

    # Atomic write
    d = os.path.dirname(path) or "."
    with tempfile.NamedTemporaryFile("w", dir=d, delete=False) as tmp:
        json.dump(out, tmp, indent=2)
        tmp.flush()
        os.fsync(tmp.fileno())
        tmp_name = tmp.name
        log.debug(f"tempfile={tmp_name} fsync() done")

    os.replace(tmp_name, path)
    try:
        # Optional: chmod for clarity (no-op if unchanged)
        os.chmod(path, 0o644)
    except Exception as e:
        log.debug(f"chmod note: {e}")
    log.debug(f"replace OK -> {path}")
