# Fays VIKit SDK Provenance

- Source: https://cnb.cool/FaysTech/FaysSenseRelease/FaysSense_VI_Kit_Release
- Branch: `main`
- Commit: `b1d74499dc48d90a6557587417577cf2b4123cad`
- Release version: `3.8.0`
- SDK version: `3.8.0`
- Synced on: `2026-07-24`

The vendored subset contains the public VIKit headers, the aarch64 and x86_64
`libfays_vikit.so` runtime libraries, and the FT602 runtime libraries required
by `standalone/FaysStereoRecorder`.

For this update, `fays_vikit.h`, `fays_vikit_version.h`, and both architecture
variants of `libfays_vikit.so` changed. The remaining vendored headers and
FT602 `1.0.17` runtime libraries were verified byte-for-byte unchanged.
