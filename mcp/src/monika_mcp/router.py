from __future__ import annotations

import asyncio
import os

import httpx2
from mcp.client.streamable_http import streamable_http_client
from mcp.server import MCPServer
from mcp.server.auth.settings import AuthSettings
from pydantic import AnyHttpUrl

from mcp import Client

from .auth import EnvironmentTokenVerifier
from .config import Instance, instance_config
from .network import resolve_bind_host


async def call(instance: Instance, tool: str, arguments: dict) -> object:
    headers = {"Authorization": f"Bearer {instance.token}"}
    async with httpx2.AsyncClient(headers=headers, follow_redirects=True) as http:
        async with streamable_http_client(instance.url, http_client=http) as transport:
            async with Client(transport) as client:
                result = await client.call_tool(tool, arguments)
                return (
                    result.structured_content
                    if result.structured_content is not None
                    else [c.model_dump() for c in result.content]
                )


def make_router(instances: list[Instance]) -> MCPServer:
    public_url = os.environ.get("MONIKA_ROUTER_PUBLIC_URL", "http://127.0.0.1:8770/mcp")
    issuer = os.environ.get("MONIKA_MCP_ISSUER_URL")
    if not issuer:
        raise RuntimeError("MONIKA_MCP_ISSUER_URL is required")
    router = MCPServer(
        "MoniKa Router",
        token_verifier=EnvironmentTokenVerifier(),
        auth=AuthSettings(
            issuer_url=AnyHttpUrl(issuer), resource_server_url=AnyHttpUrl(public_url), required_scopes=["monika:read"]
        ),
    )

    @router.tool()
    async def unlocked_instances() -> list[dict]:
        """Ask all configured MoniKa instances which are reachable and unlocked."""

        async def probe(item: Instance) -> dict:
            try:
                status = await asyncio.wait_for(call(item, "instance_status", {}), timeout=5)
                return {"name": item.name, "reachable": True, "status": status}
            except Exception:
                return {"name": item.name, "reachable": False, "status": None}

        return await asyncio.gather(*(probe(item) for item in instances))

    @router.tool()
    async def route(instance: str, tool: str, arguments: dict) -> object:
        """Route an allowed read operation to one named MoniKa instance."""
        allowed = {"list_chatrooms", "read_chatroom", "check_new_messages", "instance_status"}
        if tool not in allowed:
            raise ValueError("tool is not routable")
        target = next((item for item in instances if item.name == instance), None)
        if target is None:
            raise ValueError("unknown instance")
        return await call(target, tool, arguments)

    return router


def main() -> None:
    router = make_router(instance_config())
    host = resolve_bind_host(os.environ.get("MONIKA_ROUTER_HOST", "127.0.0.1"))
    router.run(
        transport="streamable-http",
        host=host,
        port=int(os.environ.get("MONIKA_ROUTER_PORT", "8770")),
    )


if __name__ == "__main__":
    main()
