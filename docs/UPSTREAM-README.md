# DXMT upstream origin

This repository imported DXMT's Direct3D 10/11-to-Metal implementation and develops an owned D3D12 translator on the shared foundation.
This page identifies that origin; it is not the current repository's support declaration or build recipe.

- Upstream project: [3Shain/dxmt](https://github.com/3Shain/dxmt)
- Upstream documentation: [project wiki](https://github.com/3Shain/dxmt/wiki)
- Upstream development builds: [GitHub Actions](https://github.com/3Shain/dxmt/actions)
- Imported base: `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`, followed by the frozen owned 18-patch import baseline

For this repository, use the [README](../README.md), [development guide](DEVELOPMENT.md),
and [current implementation status](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md).
The root `src/` is the development source; upstream builds do not contain this repository's current D3D12 work.

[Source provenance](provenance/source-import.json),
[retired-artifact notes](provenance/RETIRED-BUILD-EVIDENCE.md),
and [frozen build reproduction](../graphics-translator/BUILD.ko.md) preserve the import history.
Do not regenerate current sources from the frozen patches or treat historical artifact paths as available local inputs.
