from __future__ import annotations

import os
from dataclasses import asdict
from pathlib import Path

from mcp.server import MCPServer
from mcp.server.auth.settings import AuthSettings
from pydantic import AnyHttpUrl

from .auth import EnvironmentTokenVerifier
from .config import local_app_data
from .store import MonikaStore


def make_server(store: MonikaStore) -> MCPServer:
    public_url = os.environ.get("MONIKA_MCP_PUBLIC_URL", "http://127.0.0.1:8765/mcp")
    issuer = os.environ.get("MONIKA_MCP_ISSUER_URL")
    if not issuer:
        raise RuntimeError("MONIKA_MCP_ISSUER_URL is required")
    server = MCPServer(
        "MoniKa",
        token_verifier=EnvironmentTokenVerifier(),
        auth=AuthSettings(
            issuer_url=AnyHttpUrl(issuer),
            resource_server_url=AnyHttpUrl(public_url),
            required_scopes=["monika:read"],
        ),
    )

    @server.tool()
    def list_chatrooms() -> list[dict]:
        """List local chatroom databases and whether MoniKa has their DEK."""
        return [asdict(room) for room in store.rooms()]

    @server.tool()
    def read_chatroom(room_id: str, after: int = 0, limit: int = 100) -> list[dict]:
        """Read up to 500 decrypted message rows newer than a Unix timestamp."""
        return store.messages_after(room_id, after, limit)

    @server.tool()
    def check_new_messages(room_id: str, after: int) -> dict:
        """Check decrypted message rows for messages newer than a Unix timestamp."""
        return store.has_new_messages(room_id, after)

    @server.tool()
    def instance_status() -> dict:
        """Report whether this MoniKa instance currently has any unlocked rooms."""
        rooms = store.rooms()
        return {"unlocked": any(r.available for r in rooms), "available_rooms": sum(r.available for r in rooms)}

    return server


def main() -> None:
    base = local_app_data()
    cache = Path(os.environ.get("MONIKA_CACHE_DB", base / "MoniKa" / "cache.db"))
    user = os.environ.get("MONIKA_USER_DIR")
    if not user:
        raise SystemExit("MONIKA_USER_DIR must name the KakaoTalk user directory")
    server = make_server(MonikaStore(cache, Path(user)))
    server.run(
        transport="streamable-http",
        host=os.environ.get("MONIKA_MCP_HOST", "127.0.0.1"),
        port=int(os.environ.get("MONIKA_MCP_PORT", "8765")),
    )


if __name__ == "__main__":
    main()
