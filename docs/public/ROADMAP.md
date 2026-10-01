# Roadmap

This roadmap describes areas the project may explore; it does not promise dates or parity claims.

1. **Stabilize the native editor:** improve the current 2D Timeline, properties, graphs, viewer, save/reopen, media workflow, and release quality through reproducible reports and synthetic tests.
2. **Broaden native compositing:** measure and implement more effects, blend modes, mattes, masks, color handling, and render behavior while keeping unsupported states visible.
3. **Research code-to-Timeline exchange:** `.b4p` is a technical proposal. Any approved design must preserve original source, expose explicit stable bindings, and report conflicts rather than silently flattening code.
4. **Pursue wider interoperability:** general `.aep` interchange, real third-party AE plug-ins, 3D, tracking, and rotoscoping are separate high-risk tracks requiring their own permission, architecture, and evidence.
5. **Consider other platforms only after measurement:** each port needs a verified build, media/render path, and supported test suite.

The 0.7.0 alpha does not include the proposed format, engine adapters, or full AE parity.
