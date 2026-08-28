from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Instance:
    name: str
    url: str
    token: str


def local_app_data() -> Path:
    value = os.environ.get("LOCALAPPDATA")
    if not value:
        raise RuntimeError("LOCALAPPDATA is not set")
    return Path(value)


def instance_config() -> list[Instance]:
    path = os.environ.get("MONIKA_MCP_INSTANCES")
    if not path:
        raise RuntimeError("MONIKA_MCP_INSTANCES must point to a protected JSON file")
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    result = []
    for item in data.get("instances", []):
        url = str(item["url"]).rstrip("/")
        if (
            not url.startswith("https://")
            and not url.startswith("http://127.0.0.1")
            and not url.startswith("http://localhost")
        ):
            raise ValueError(f"instance {item['name']!r} must use HTTPS (except loopback)")
        result.append(Instance(str(item["name"]), url, str(item["token"])))
    return result
