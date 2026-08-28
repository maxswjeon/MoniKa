from __future__ import annotations

import hashlib
import hmac
import json
import os
import time

from mcp.server.auth.provider import AccessToken, TokenVerifier


class EnvironmentTokenVerifier(TokenVerifier):
    """Verify opaque bearer tokens from MONIKA_MCP_TOKENS_JSON.

    The JSON maps SHA-256 token digests to {client_id, scopes, expires_at}.
    Keeping only digests avoids storing bearer credentials in configuration.
    """

    async def verify_token(self, token: str) -> AccessToken | None:
        configured = json.loads(os.environ.get("MONIKA_MCP_TOKENS_JSON", "{}"))
        digest = hashlib.sha256(token.encode()).hexdigest()
        match = next((v for k, v in configured.items() if hmac.compare_digest(k, digest)), None)
        if not match or int(match.get("expires_at", 0)) <= int(time.time()):
            return None
        return AccessToken(
            token=token,
            client_id=str(match["client_id"]),
            scopes=list(match.get("scopes", [])),
            expires_at=int(match["expires_at"]),
        )
