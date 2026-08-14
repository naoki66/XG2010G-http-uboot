# XG2010G U-Boot HTTP Recovery

This repository contains the XG2010G (Airoha AN7581 / ECONET EN7581)
U-Boot HTTP recovery port, based on [YYH2913's http-uboot](https://github.com/YYH2913/http-uboot).

The project includes the recovery server, chainloader support, board support,
and reproducible GitHub Actions builds. Build artifacts are published by the
workflow; device-specific recovery procedures and factory images are kept out
of this repository.

## Development

Use the repository's normal U-Boot build flow. The CI workflow can be started
manually from GitHub Actions and publishes SHA-256 checksums for its artifacts.

All hardware flashing is performed at the user's own risk. Verify the target
device and recovery method before writing any persistent storage.
