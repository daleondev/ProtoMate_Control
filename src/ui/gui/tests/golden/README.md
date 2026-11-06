# Reviewed HMI screenshots

These are isolated presentation fixtures, not proof of robot motion. The separate
`simulation.hmi` test drives the actual frontend, ICC and native robot plant.

`gui.integration` generates the BMP inputs. `gui.screenshots` compares the stable
pixels against these reviewed PNGs. `regions.json` excludes asynchronous 3D image
completion and changing graphics counters, but restores the overlaid captions and
controls to the comparison. Mesh/transform/ownership tests cover the renderer;
the complete images are retained for visual review.

Do not regenerate these files just to make a failed test pass. Inspect the changed
page, layout and text at 1024×600 first, including states and the persistent Stop.
