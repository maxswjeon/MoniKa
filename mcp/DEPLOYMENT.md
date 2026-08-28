# Recommended router deployment

The recommended MoniKa MCP layout keeps decrypted chat access on each KakaoTalk
computer and makes one authenticated `monika-mcp-router` endpoint available to
MCP clients through a private tailnet.

```text
MCP clients
    |
    | HTTPS + router bearer token
    v
Tailscale Serve / tailnet ACL
    |
    | loopback HTTP
    v
monika-mcp-router
    |
    | HTTPS + a different token for each instance
    +-----------> monika-mcp on desktop A -> local KakaoTalk data
    +-----------> monika-mcp on desktop B -> local KakaoTalk data
```

Only the router should be shared with general MCP clients. Each data instance
should remain local or be reachable only from the router identity under a
tailnet ACL. DEKs remain inside the instance process and are never returned by
an MCP tool.

## Recommended: loopback plus Tailscale Serve

Run the router on its default loopback address and let Tailscale Serve terminate
HTTPS. Replace the example MagicDNS name with the hostname reported by
`tailscale serve status`.

```powershell
$env:MONIKA_ROUTER_HOST = "127.0.0.1"
$env:MONIKA_ROUTER_PORT = "8770"
$env:MONIKA_ROUTER_PUBLIC_URL = "https://router.example-tailnet.ts.net/mcp"
$env:MONIKA_MCP_ISSUER_URL = "https://auth.example-tailnet.ts.net/"
$env:MONIKA_MCP_INSTANCES = "C:\ProgramData\MoniKa\instances.json"
$env:MONIKA_MCP_TOKENS_JSON = '{...router token digests...}'

monika-mcp-router
tailscale serve --bg --https=443 http://127.0.0.1:8770
```

Tailscale Serve keeps the backend off the LAN and tailnet interfaces while
providing a tailnet-only HTTPS endpoint. Do not use Tailscale Funnel: it makes
the endpoint public.

Apply a tailnet grant or ACL that permits only approved user or device
identities to connect to the router on TCP 443. Tailnet authorization is an
outer control; the router bearer token and `monika:read` scope remain required.

## Direct tailnet binding

For a controlled environment with TLS supplied separately, the router can bind
directly to the current node's Tailscale IPv4 address:

```powershell
$env:MONIKA_ROUTER_HOST = "tailnet"
$env:MONIKA_ROUTER_PUBLIC_URL = "https://router.example-tailnet.ts.net/mcp"
monika-mcp-router
```

The special `tailnet` value runs `tailscale ip -4`, accepts exactly one address
from Tailscale's `100.64.0.0/10` range, and fails closed if Tailscale is missing,
stopped, or ambiguous. Binding directly does not provide HTTPS by itself. Do not
send bearer tokens to a plain HTTP endpoint; put a TLS reverse proxy in front or
use the recommended Tailscale Serve layout.

## Instance credentials

Store `instances.json` outside the source tree with an ACL readable only by the
router service account. Use a distinct, short-lived bearer token for every
instance so one credential can be revoked without rotating all links. Router
client tokens must also be separate from upstream instance tokens.

An instance entry uses an HTTPS URL and its dedicated token:

```json
{
  "instances": [
    {
      "name": "desktop-a",
      "url": "https://desktop-a.example-tailnet.ts.net/mcp",
      "token": "instance-specific-secret"
    }
  ]
}
```

For a small private deployment, the built-in SHA-256 token-digest map is
appropriate when tokens are random, scoped, expiring, and distributed safely.
Use signed JWT validation or OAuth introspection when centralized lifecycle and
auditing are required.

## Operational checklist

- Run every process under a dedicated, unprivileged account.
- Restrict `instances.json`, environment files, logs, and KakaoTalk data with
  filesystem ACLs.
- Permit the router to reach instance MCP ports; deny client identities direct
  access to those ports.
- Keep both MCP bearer authentication and tailnet policy enforcement enabled.
- Back up configuration, but never copy dumps, DEKs, or live database material
  to the router host.
- Verify `unlocked_instances` before routing a request, and treat unreachable or
  locked instances as unavailable rather than retrying indefinitely.
