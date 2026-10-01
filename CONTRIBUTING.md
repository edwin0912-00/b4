# Contributing

Thank you for helping improve B4. This is an early native macOS alpha; small fixes and focused regression tests are the easiest contributions to review.

## Before starting

- Search existing issues and open an issue before a large behavior change.
- Keep pull requests focused. Describe the affected control and the expected behavior.
- Do not submit Adobe source, proprietary artwork, private user projects, customer media, credentials, or binaries with unknown provenance.
- For new code, fixtures, fonts, and dependencies, identify the source and license. Discuss new runtime dependencies before adding them.
- Do not present a partial feature as full After Effects parity. `.aep`, native AE plug-ins, `.b4p`, and other platforms are not supported by this alpha.

## Build and test

Use the commands in [Build](docs/public/BUILD.md). Run the full CTest suite before submitting a behavior change:

```sh
ctest --test-dir build --output-on-failure
```

For a UI change, include the affected UI check and a short description of how you exercised the control. If a test cannot run on your machine, say why; do not report it as passing.

## Good first contributions

- Fix a small reproduced bug with a synthetic project and a regression test.
- Improve documentation for an existing 0.7 control without making unverified AE-equivalence claims.
- Add accessibility or keyboard coverage for an existing native UI action.

Please review the [Code of Conduct](CODE_OF_CONDUCT.md) and [Security policy](SECURITY.md). Contributions to the original project code are made under [MPL-2.0](LICENSE). Confirm that you have the right to submit your contribution; separately licensed material must retain its original license and be identified for review.
