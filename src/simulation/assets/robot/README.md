# Fusion robot visuals

The fixed base and three articulated links are the actual Fusion exports:

- `base_link/base_link.dae`
- `link_1/link_1.dae`
- `link_2/link_2.dae`
- `link_3/link_3.dae`

All four are embedded with C++26 `#embed` and loaded from memory by Assimp.
The application needs no asset directory at runtime. Materials are imported;
external textures and skeletal animation are unsupported. The supplied files
use material colors and rigid bodies. The adjacent `*_collision.stl` files are
reserved for future work and never selected or loaded by this implementation.

Each export declares metres and Z-up. Assimp normalizes COLLADA Z_UP to Y-up;
`ModelCoordinates` then converts that imported scene to millimetres/Z-up.
Do not apply the original Z-up metadata again or scale the meshes to fit link
lengths. Link registration and tool-tip placement were checked against the
full STEP assembly; see [the geometry report](../../../../docs/robot/geometry.md).

For inspecting updated visual exports without recompiling:

```sh
./build/debug-linux/Application --robot-models src/simulation/assets/robot
```

That directory must contain the four exact paths above with the same local
frames. Missing/invalid requested visuals produce an explicit error; there is
no dummy fallback. `--model-scale` and `--model-up` apply to **post-Assimp**
coordinates of external overrides (defaults 1000 mm/unit and Y-up). They do
not change embedded asset metadata, physical geometry, or joint positions.
Changing mesh origins requires reviewing the STEP registration, not a camera
or scale workaround. `scripts/extract_robot_geometry.py` regenerates the
measurement evidence; `simulation.cad_assets` detects stale source hashes.
