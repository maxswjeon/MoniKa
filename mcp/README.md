# MoniKa MCP

This directory provides two authenticated Streamable HTTP MCP services:

- `monika-mcp`: lists chatroom databases, reports DEK availability, reads bounded decrypted message rows, and checks for messages after a Unix timestamp.
- `monika-mcp-router`: concurrently probes several instances and routes an allow-listed read tool to a named instance.

Raw DEKs never appear in MCP responses. Both services require the `monika:read` bearer-token scope. They bind to loopback by default; use a TLS reverse proxy for remote access. Plain HTTP instance URLs are rejected except on loopback.

## Install

From the repository root:

```powershell
uv sync --all-packages
uv run --package monika-mcp monika-mcp
```

Create an opaque token, then configure its SHA-256 digest (not the token itself):

```powershell
$token = python -c "import secrets; print(secrets.token_urlsafe(32))"
$digest = python -c "import hashlib,sys; print(hashlib.sha256(sys.argv[1].encode()).hexdigest())" $token
$env:MONIKA_MCP_TOKENS_JSON = '{"' + $digest + '":{"client_id":"local-client","scopes":["monika:read"],"expires_at":1893456000}}'
```

For an instance, set `MONIKA_USER_DIR` and `MONIKA_MCP_ISSUER_URL`; optionally set `MONIKA_CACHE_DB` and `MONIKA_MCP_PUBLIC_URL`, then run `monika-mcp`. The issuer is mandatory so a deployment cannot accidentally advertise a fake authorization server.

For the router, copy `instances.example.json` outside the repository, restrict its filesystem ACL because it contains upstream bearer tokens, set `MONIKA_MCP_INSTANCES` to that path, and run `monika-mcp-router`. Router callers use a separate token configured through `MONIKA_MCP_TOKENS_JSON`.

The included opaque-token verifier is suitable for a small private deployment when tokens are generated randomly, scoped, short-lived, and distributed securely. For centrally managed deployments, replace it with JWT signature/audience validation or RFC 7662 introspection against your OAuth 2.1 authorization server. Configure real issuer/resource URLs and terminate TLS at a trusted reverse proxy. Do not expose either default loopback HTTP port directly.
