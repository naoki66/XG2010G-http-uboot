# XG2010G U-Boot HTTP Recovery

This repository contains the XG2010G (Airoha AN7581 / ECONET EN7581)
U-Boot HTTP recovery port, based on [YYH2913's http-uboot](https://github.com/YYH2913/http-uboot).

The project includes the recovery server, chainloader support, board support,
and reproducible GitHub Actions builds. Build artifacts are published by the
workflow; device-specific recovery procedures and factory images are kept out
of this repository.

## Development

### Local build artifacts

Chainloader builds always use the fixed `out/` directory. Canonical files carry
the local timestamp and Git revision, for example:

```text
out/xg2010g-u-boot-YYYYMMDD-HHMMSS-g<commit>.bin
out/xg2010g-chainloader-YYYYMMDD-HHMMSS-g<commit>.itb
out/xg2010g-chainloader-YYYYMMDD-HHMMSS-g<commit>-slot.bin
```

The stable aliases `out/xg2010g-chainloader.itb` and
`out/xg2010g-chainloader-slot.bin` are retained for existing scripts.

Use the repository's normal U-Boot build flow. The CI workflow can be started
manually from GitHub Actions and publishes SHA-256 checksums for its artifacts.

All hardware flashing is performed at the user's own risk. Verify the target
device and recovery method before writing any persistent storage.
