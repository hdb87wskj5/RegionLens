# Source provenance

- Product: RegionLens 1.3.0, Stable channel.
- Maintained source: private `RegionLens-Unified` repository, commit
  `204f18259c8ea4279900a6ec108ed55b73d8d17b`.
- This repository starts with a new Git history. It does not contain the
  maintained repository's earlier commits or signing artifacts.
- `src/Core` was exported unchanged. The Stable application, installer and
  WeType compatibility source came from the same commit; build entry points
  were narrowed to the Stable channel and Dev-only code was omitted.
- A few dormant Dev identity/upgrade branches remain inside the shared core
  and installer source so the Stable implementation stays faithful to the
  reviewed 1.3.0 code. No Dev executable, recorder or build target is present.
- Future changes should be made in `RegionLens-Unified`, then exported as a
  reviewed new snapshot. Do not independently implement product fixes here.

Export selection: `src/Core`, `src/App`, `src/Setup`, `src/WeTypeProbe`,
`build`, and production-relevant `tests`. Exclusions: diagnostic writer,
InputProbe, diagnostic/incident tests and fixtures, Dev-only build scripts,
real-machine performance notes, historical build outputs and certificates.
