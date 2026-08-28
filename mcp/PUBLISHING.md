# Publishing `monika-mcp`

Releases use PyPI Trusted Publishing. Do not create a PyPI API token or a
`PYPI_TOKEN` GitHub secret.

## Release

1. Update `version` in `mcp/pyproject.toml` and refresh `uv.lock` with
   `uv lock`.
2. Commit and push the version change.
3. Create and push an identical `v`-prefixed tag. For version `0.1.0`:

   ```powershell
   git tag -s v0.1.0 -m "MoniKa 0.1.0"
   git push origin v0.1.0
   ```

The release workflow rejects a tag that differs from the Python package
version. It builds the distributions in an unprivileged job, transfers them as
a GitHub artifact, and grants `id-token: write` only to the PyPI publishing job.
