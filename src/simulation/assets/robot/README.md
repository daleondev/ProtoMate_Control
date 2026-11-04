# Fusion robot visuals

The fixed base and three articulated links are the actual Fusion exports:

- `base_link/base_link.dae`
- `link_1/link_1.dae`
- `link_2/link_2.dae`
- `link_3/link_3.dae`

All four are embedded with C++26 `#embed` and loaded from memory by Assimp.
The application needs no asset directory at runtime. Materials are imported;
external textures and skeletal animation are unsupported. The supplied files
use material colors and rigid bodies. The moving links' adjacent `*_collision.stl` files are embedded separately for
native home-switch contact detection; they are never used as visual meshes.
The fixed base STL is retained for future general collision work. Collision
STLs use metres/Z-up in the same local frames, without Assimp normalization.
`sensors/*_lever.stl` contains the three named free switch levers extracted from
the parent DAEs by `scripts/extract_robot_switches.py`. `sensors/sources.json`
records source and output hashes, including the moving collision models.
Runtime validates closed manifold topology, finite geometry and STL lengths.

The straight exported pose is the simulation startup zero: 0°/0° rotary joints
and 0 mm Z travel. TCP height is 28.9 mm at that pose. Rendering and switch
collision transforms both come from the native `Kinematics` implementation;
motor conversion reuses the UI's pure axis conversion classes. Electrical switch
travel remains an explicit nominal profile assumption; see the simulator README.

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
