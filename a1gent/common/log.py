# a1gent/common/log.py
import logging, os, sys

def setup_logger(name: str) -> logging.Logger:
    level_name = os.getenv("PY_AGENT_LOG", "INFO").upper()
    level = getattr(logging, level_name, logging.INFO)

    logger = logging.getLogger(name)
    if logger.handlers:
        return logger

    logger.setLevel(level)
    h = logging.StreamHandler(sys.stdout)
    h.setFormatter(logging.Formatter("%(asctime)s | %(levelname)s | %(name)s | %(message)s",
                                     "%H:%M:%S"))
    logger.addHandler(h)
    logger.propagate = False
    return logger
