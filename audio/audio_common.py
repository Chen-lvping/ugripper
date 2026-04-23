from __future__ import annotations

from datetime import datetime


def audio_log(level: str, message: str) -> None:
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"[audio][{level}][{timestamp}] {message}", flush=True)


def read_env_file_value(key: str, env_file: str = "/etc/environment") -> str | None:
    try:
        with open(env_file, "r", encoding="utf-8") as file:
            for raw_line in file:
                line = raw_line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                current_key, value = line.split("=", 1)
                if current_key.strip() != key:
                    continue
                return value.strip().strip('"').strip("'")
    except Exception as exc:
        audio_log("WARN", f"failed to read {env_file}: {exc}")
    return None
