# Bundled Applepie SDK

This directory contains the five header files required to compile EndfieldAntiKick.
They are copied without modification from [Sasye/ApplepieManager](https://github.com/Sasye/ApplepieManager)
commit `e5facbf7f3716f8abff2025061819983fcebb0f3`, directory `sdk/`, on 2026-10-15.
The SDK ABI is 1.1.

- `applepie.h`: public C API.
- `applepie_client.h`: C++ client helpers.
- `applepie_native.h`: native entry helpers.
- `applepie_seh.h`: exception helpers.
- `applepie_pointer_scope.h`: pointer patch scope helpers.

The upstream license is included verbatim in `LICENSE` (GNU AGPL version 3).
`VERSION.json` records the source commit and SHA256 hashes of the copied files.
Git attributes preserve those files byte for byte across checkouts.

Run `build.bat` from the project root to use this SDK. The compiler only needs
these headers, MSVC and the Windows SDK; no ApplepieManager source checkout,
client library or MinHook library is needed. A compatible ApplepieManager is
still required to load and run the resulting plugin.

To update the snapshot, review upstream API and helper changes, replace all five
headers and the license together, update `VERSION.json`, then rebuild and
validate compatibility. `APPLEPIE_SDK_DIR` can select an external SDK for development.
