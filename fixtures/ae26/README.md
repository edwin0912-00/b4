# AEP parser regression fixtures

These are sanitized copies of two locally generated synthetic After Effects
26.2.1x2 projects. The original fixture note records that no user project was
reused. The copies exist only as inputs to the offline `aep_probe` regression;
they do not make the native app an AEP importer or establish general AEP
compatibility.

For each copy, only the parsed image alias changed: its machine-specific absolute
`fullpath` became `fixtures/original-image.png`. JSON whitespace before the
object's closing brace preserves the `alas` chunk size. The basename remains
`original-image.png`, all other fields returned by `scripts/aep_probe.py` are
unchanged, and each opaque XMP trailer is byte-for-byte identical to its source.

The opaque trailers were inspected. They contain the AE creator/version,
creation and save timestamps, document and instance UUIDs, and save-history
entries. No personal or account names, session fields, or filesystem paths were
present. The unknown trailer bytes were retained unchanged.

| Fixture | Bytes | Source SHA-256 | Sanitized SHA-256 |
| --- | ---: | --- | --- |
| `synthetic.aep` | 82,102 | `428cba1e1c805ed5c5d9066ea9b138a14d34107e83b14ca992ac081a0f50468e` | `4a44065d2b582d176c0fa58f9e556325a4d41fb953da62698a09966ab75ddd11` |
| `authored-variant.aep` | 83,660 | `7a4405707002393ab77804f41248897b07e7fe8681544965279f7b8e9f94790d` | `2f45ffbe6c53dac67b64fa5241d5eda3909c23e75986c1d26594619f7a714397` |

Run the offline parser checks with `python3 -B tests/aep_probe_checks.py`. The
probe covers only its documented measured subset and preserves unknown bytes;
it is not a general project reader or renderer.
