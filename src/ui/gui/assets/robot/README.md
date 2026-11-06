# Embedded robot visuals

These are **visual-only derivatives** of the four Fusion DAE exports in
`src/simulation/assets/robot`. Originals, collision STLs and switch contact
geometry are unchanged. Do not use these simplified surfaces for collision
detection, mechanical measurements or path certification.

| Link | Original triangles | Prepared triangles |
| --- | ---: | ---: |
| Fixed base | 177,618 | 11,519 |
| Shoulder | 54,834 | 8,446 |
| Elbow / stationary Z stage | 324,056 | 13,506 |
| Moving Z carriage / tool | 66,784 | 2,905 |
| **Total** | **623,292** | **36,376** |

The reduction is **94.16%**. This quality-preserving version is larger than the
initial few-thousand-triangle aspiration. It is an asset preparation result,
not qualification of the GIGA viewport's frame rate or display bandwidth.

## Coordinates and appearance

- Metres, right-handed Z-up, original link origins. No automatic centering,
  normalization, axis conversion, or assembly-pose baking.
- Each OBJ is one rigid link, with the original opaque material colours and
  explicit corner normals. No textures or texture coordinates.
- Small threaded brass inserts are intentionally omitted as visual detail.
  `manifest.json` lists every omitted body. Visible screw heads, structural
  openings, switch levers and the cutter tip remain represented.
- Coplanar dissolution and bounded edge-collapse attempts simplify the retained
  bodies. The deterministic sampled, bidirectional surface-distance check uses
  0.75 mm; this is not a certified Hausdorff bound or a bound on omitted detail.
- An exterior-visibility pass removes faces hidden within **the same rigid
  link**. It samples four points and outward directions, then retains a face
  adjacency ring to protect openings. Other links never occlude this test, so
  a folded pose cannot cause faces to be removed from an adjacent link.
  Sampling is conservative visual processing, not proof of all-view visibility.
- Normals are transferred after topology changes, with hard-edge protection.
  The full assembled model is visually reviewed from above, below and around
  three poses. These assets target a whole-robot overview; close-up machining
  detail is intentionally not represented faithfully.

Source/output hashes, bounds, measured simplification deviations and tool
version are recorded in `manifest.json`. The CAD geometry report under
`docs/robot/geometry.md` remains authoritative for link registration.

## Reproduce and review

Offline preparation uses **Blender 4.0.2**, including its Python API. The normal
CMake build requires only the Python dependencies in `scripts/requirements.txt`.
Do not silently substitute a Blender version: modifier results and normal APIs
can differ. From the repository root:

```sh
mkdir -p build/robot-visuals
blender --background --factory-startup \
  --python scripts/prepare_robot_visuals.py -- \
  --blend build/robot-visuals/final.blend

blender --background build/robot-visuals/final.blend \
  --python scripts/review_robot_visuals.py -- \
  --output build/robot-visuals/views

cmake --build --preset debug-linux --target scene3d_robot_tests scene3d_tests
ctest --test-dir build/debug-linux -R 'gui.scene3d' --output-on-failure

python3 scripts/report_robot_visuals.py \
  --views build/robot-visuals/views \
  --compiled build/debug-linux/generated/scene3d/robot
```

The matched comparisons use 640×400 images, 12 camera directions and three
articulated poses. All 36 reviewed comparisons have silhouette intersection
over union ≥99.868%. This measures agreement for these views, not all possible
camera positions. Contact sheets and hash-bound measurements are in
[`docs/robot/visuals`](../../../../../docs/robot/visuals).

## Build and consume in scene3d

```sh
cmake --build --preset debug-linux --target scene3d-robot-assets
cmake --build --preset giga-ui-release --target scene3d-robot-assets
```

Link `proto::ui::gui::robot_assets`. Generated data resides in
`build/<preset>/generated/scene3d/robot`, never in the vendor submodule. The
library is opt-in on hardware; these assets do not enable robot rendering or
change the normal startup screen. M4 does not link them.

```cpp
#include "robot_link_1_asset.hpp"

scene.nodes[i].mesh = &ui::gui::scene3d::assets::robot_link_1;
scene.nodes[i].material.useMeshMaterial = true;
```

The four exported names are `robot_base_link`, `robot_link_1`, `robot_link_2`
and `robot_link_3`. One node per link retains all material colours; there is no
need to allocate one node per CAD body or colour. Existing nodes keep their
override colour unless `useMeshMaterial` is enabled explicitly.

Use the existing simulator/kinematics' link transforms when connecting real
state. The renderer test supplies documented nominal CAD poses only; it adds
no duplicate production kinematics or motion logic.

TGX conversion retains watertight shared-vertex quantization. Link 1 uses
0.001 bounding-box padding; the other links use 0.0002. The pinned exporter's
default lattice does not converge for link 1. These are offline encoding
settings, not scaling of the link or disabling seam checks. Conversion reports
include the additional micrometre-scale vertex displacement.

For actual TGX RGB565 captures of the prepared meshes:

```sh
SCENE3D_CAPTURE_DIR=build/robot-visuals/tgx \
  build/debug-linux/src/ui/gui/scene3d/scene3d_robot_tests
```

The tests cover deterministic articulated rendering, per-link material colours,
texture exclusion, preserved tool/base coordinates and buffer guards. Python
tests verify hashes, finite vertices, normal lengths, triangle validity, bounds
and rejection of unexpected source transforms/units.
