from __future__ import annotations

import subprocess
from types import SimpleNamespace

import pytest
from monika_mcp.network import resolve_bind_host


def test_regular_bind_host_is_unchanged() -> None:
    assert resolve_bind_host("127.0.0.1") == "127.0.0.1"


def test_tailnet_resolves_tailscale_ipv4(monkeypatch: pytest.MonkeyPatch) -> None:
    result = SimpleNamespace(returncode=0, stdout="100.101.102.103\n", stderr="")
    monkeypatch.setattr(subprocess, "run", lambda *args, **kwargs: result)

    assert resolve_bind_host("tailnet") == "100.101.102.103"


def test_tailnet_rejects_non_tailscale_address(monkeypatch: pytest.MonkeyPatch) -> None:
    result = SimpleNamespace(returncode=0, stdout="192.168.1.5\n", stderr="")
    monkeypatch.setattr(subprocess, "run", lambda *args, **kwargs: result)

    with pytest.raises(RuntimeError, match="unique Tailscale IPv4"):
        resolve_bind_host("tailnet")


def test_tailnet_reports_missing_cli(monkeypatch: pytest.MonkeyPatch) -> None:
    def missing(*args: object, **kwargs: object) -> None:
        raise FileNotFoundError

    monkeypatch.setattr(subprocess, "run", missing)

    with pytest.raises(RuntimeError, match="Tailscale installed"):
        resolve_bind_host("tailnet")
