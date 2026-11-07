# Reviewed HMI screenshots

These are isolated presentation fixtures, not proof of robot motion. The separate
`simulation.hmi` test drives the actual frontend, ICC and native robot plant.

`gui.integration` and `gui.pages` generate the BMP inputs. `gui.screenshots` compares the stable
pixels against these reviewed PNGs. `regions.json` excludes asynchronous 3D image
completion and changing graphics counters, but restores fixed scene controls and
the provenance prefix to the comparison. Frame-matched badges are covered by
projection/metadata/ownership tests rather than timing-dependent pixel goldens.
Pixel-based checks additionally verify centering of tabs, large glyphs, multiline
fields and keypad keys. Mesh/transform/ownership tests cover the renderer;
the complete images are retained for visual review.

Do not regenerate these files just to make a failed test pass. Inspect the changed
page, layout and text at 1024×600 first, including states and the persistent Stop.
