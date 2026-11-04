# Robot viewer assets

These three GLB files are **synthetic placeholders**, not exports of
`docs/robot/SCARA_Robot.f3d`. They exercise the same Assimp import, indexed GPU
upload, material and articulation path as future CAD exports. Regenerate them
with `python3 src/simulation/assets/robot/generate_placeholders.py`; generation
is not a build step. Their generator uses only the Python standard library.

The assets are embedded into the native Linux executable with C++26 `#embed`
at build time. No
asset directory, companion binary, Fusion installation or asset extraction is
needed to run the default `Application`. System Assimp, epoxy, GLM and SDL2
remain build dependencies; their shared libraries remain runtime dependencies.
The original Fusion document is not modified.

## Replacing them with exports

Put one supported mesh file for each moving group in a directory:

| File stem | Rigid group | Local origin | Local +X |
|---|---|---|---|
| `shoulder` | All parts that rotate with joint 1, before joint 2 | Shoulder axis at arm mounting height | Toward elbow at zero joint angle |
| `forearm` | All parts that rotate with joint 2, including the stationary Z guide | Elbow axis at the same mounting height | Toward Z attachment at zero joint angle |
| `carriage` | Moving Z carriage, tool bracket and tool | Z attachment, at TCP height | Toward TCP |

In the robot's local frame +Z is up. The TCP is `(135, 0, 0)` mm in the carriage
frame for the current provisional profile. Include vertical tool offsets in the
mesh placement so the actual tip meets that point. Shoulder-to-elbow and
elbow-to-Z offsets come from the simulation profile: 205 and 223.4 mm.

Export each group in its own local frame and zero rotary pose, **not all three
in the original world coordinates**. Preserve the relative transforms of parts
inside each group. Embedded GLB dummy assets use the normal glTF metre/Y-up
convention; their root transforms convert the millimetre/Z-up authoring frame.

For self-contained GLB exports in metres/Y-up:

```sh
./build/debug-linux/Application --robot-models /absolute/path/to/exports
```

For Fusion OBJ or STL exports explicitly selected as millimetres/Z-up:

```sh
./build/debug-linux/Application --robot-models /absolute/path/to/exports \
  --model-scale 1 --model-up z
```

`--model-scale` means **millimetres per imported scene unit**; default 1000.
`--model-up` defaults to `y`. These settings apply to all three files. File
extension alone does not establish scale or axes. FBX importers may carry unit
conversion in node transforms; inspect the result before using it as a visual
reference. OBJ sidecar MTL files must remain beside their OBJ files. Choose
solid material colors when exporting; texture maps are not implemented.

`--model-shoulder-height MM` sets the shoulder mounting height (default 140 mm).
It moves both rotary-link frames while keeping the carriage TCP's Z equal to
the simulation's Z coordinate. It is a rendering datum, not a change to robot
calibration, gearing, kinematics or motion limits. Meshes are never stretched
to make an incompatible export fit the current mechanical profile.

Supported file types are determined by the installed Assimp build. Exactly one
supported file per stem is required. Missing, ambiguous or invalid models cause
a diagnostic and the viewer stays unavailable; simulation continues headless.
There is no silent substitution of dummy geometry for a requested CAD export.
Fusion `.f3d`/`.f3z` archives must be exported first.

The stationary base, world axes and TCP marker remain procedural scaffolding.
The three mesh groups follow actual simulated shaft-derived joint state,
including turns beyond one revolution and Z motion. No mesh animation track,
skinning, collision, contact or dynamics is used. Loading meshes does not
calibrate the simulation or validate physical robot accuracy.

## Rendering and limits

Assimp scene hierarchy and repeated mesh instances are preserved. Normals are
imported or generated, transformed with the inverse transpose, and normalized.
CAD base/diffuse material colors and opacity are rendered. Translucent instances
are sorted back-to-front; intersecting transparent objects are not rendered
with order-independent transparency. Texture maps emit a warning and use
material colors instead. Animation/skinning and non-triangle primitives are
rejected. Lights and cameras in the asset are unused.

Limits per part: 256 MiB input, 4096 meshes/materials, 2 million vertices,
6 million indices, 16384 hierarchy nodes/instances, hierarchy depth 128, and
10 million instanced vertices. Non-finite or singular transforms, invalid
indices/normals/materials and transformed coordinates beyond 10 metres are
rejected. Limits apply to imported data; Assimp still parses the input before
post-import validation. Treat exports as trusted local project assets.

`simulation.models` covers import, units, hierarchical transforms, instancing,
materials, file overrides, malformed inputs and agreement of rendered joint
frames with FK. `simulation.opengl` draws the imported geometry and checks
framebuffer changes for all three axes under Xvfb/Mesa.
