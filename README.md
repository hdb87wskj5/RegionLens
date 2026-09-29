# RegionLens 1.3.0

RegionLens (区域镜) mirrors a selected part of the Windows desktop into a
resizable, optionally full-screen window. It supports multiple regions,
screenshots, image-quality controls and guarded mouse mapping. This repository
contains the **Stable-channel source**, not an official signed installer.

## Requirements and build

- x64 Windows 11
- Visual Studio 2026 with the C++ desktop tools (v145 toolset)
- Windows SDK 10.0.26100.0 and Windows PowerShell

Run from a PowerShell window in the repository:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-Unsigned.ps1 -Configuration Debug
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-Unsigned.ps1 -Configuration Release
```

Each command compiles the Stable app and `RegionLens-WeTypeCompat.dll`, then
runs the automated core tests using fake input backends. Output is under
`out/`. The build does not create certificates, change trust stores, install
the app or inject automated mouse input into the real desktop.

The unsigned build is useful for development and review, **not** a complete
UIAccess deployment. Windows requires an appropriately signed executable
installed in a secure location for the full mouse-mapping feature. The
installer source is included, but this repository has no release signing key
and the default build does not produce an installer. Anyone packaging their
own build must use a certificate they control, review the installer's trust
behavior, and clearly distinguish their package from an official release.

## Source layout

- `src/Core`: capture, rendering, window UI, input mapping and safe recovery.
- `src/App`: Stable entry point and application resources.
- `src/WeTypeProbe`: optional, identity-checked WeType candidate-window
  compatibility component; it silently remains inactive on unsupported
  versions.
- `src/Setup`: signed-installation and uninstall source.
- `tests`: production-core tests; no InputProbe or runtime diagnostic writer.

The source was exported from the maintained unified tree. See
[`SOURCE_PROVENANCE.md`](SOURCE_PROVENANCE.md) for the exact source commit.
Local signing material, binaries, crash dumps and runtime logs are not part
of this repository. No installer or release binary is published here.

## License

The project's original code and supplied kangaroo icon are MIT licensed;
see [`LICENSE`](LICENSE). The adapted AMD CAS shader retains its own MIT notice
in `src/Core/QualityShaders.h`; see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
