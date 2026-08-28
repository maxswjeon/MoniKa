from __future__ import annotations

import ipaddress
import subprocess

_TAILSCALE_IPV4 = ipaddress.ip_network("100.64.0.0/10")


def resolve_bind_host(configured_host: str) -> str:
    """Resolve the special ``tailnet`` bind target to this node's Tailscale IPv4."""
    if configured_host.casefold() != "tailnet":
        return configured_host

    try:
        result = subprocess.run(
            ["tailscale", "ip", "-4"],
            capture_output=True,
            check=False,
            text=True,
            timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError("cannot resolve tailnet binding; is Tailscale installed and running?") from exc

    if result.returncode != 0:
        detail = result.stderr.strip() or "tailscale ip -4 failed"
        raise RuntimeError(f"cannot resolve tailnet binding: {detail}")

    addresses: list[ipaddress.IPv4Address] = []
    for value in result.stdout.split():
        try:
            address = ipaddress.IPv4Address(value)
        except ipaddress.AddressValueError:
            continue
        if address in _TAILSCALE_IPV4:
            addresses.append(address)

    if len(addresses) != 1:
        raise RuntimeError("cannot resolve a unique Tailscale IPv4 address")
    return str(addresses[0])
