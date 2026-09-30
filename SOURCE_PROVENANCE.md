# Source provenance

- Product: RegionLens 1.3.0, Stable channel.
- Maintained source: private `RegionLens-Unified` repository, commit
  `b210c3536905c3b2158dbd0485763f25495196cf`.
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

The 1.3.0 settings fix keeps Settings modeless: opening it no longer stops
mapping, releases global hotkeys or blocks region creation. The shared core
matches the maintained commit above. The public entry point retains only
Stable composition; the version is unchanged. Corresponding fake-input and
native settings regression tests are included, not shipped in the application.

Export selection: `src/Core`, `src/App`, `src/Setup`, `src/WeTypeProbe`,
`build`, and production-relevant `tests`. Exclusions: diagnostic writer,
InputProbe, diagnostic/incident tests and fixtures, Dev-only build scripts,
real-machine performance notes, historical build outputs and certificates.
