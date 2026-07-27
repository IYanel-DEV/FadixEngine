# Graphics validation scene

Repeatable scene for comparing **Scene View @ Low**, **Game View @ High (edit)**, and **Game View @ High (Play)**. Used with `artifacts/graphics-validation/RESULTS.md` and the three PNG captures.

## Contents

| Entity | Purpose |
|--------|---------|
| **Sun Light** | Directional key light, cascaded shadows (2048) |
| **Fill Spot** | Local spot fill — enable **Cast Shadows** in Inspector if not already on |
| **Rim Point** | Point accent light |
| **Environment** | Primary env: bloom on (threshold 0.75, intensity 0.45), tonemap, exposure 1.0 |
| **Ground** | Large plane — depth range for cascades / SSAO |
| **White Albedo Cube** | `(1,1,1)` base color, low roughness — preset color consistency check |
| **Emissive Sphere** | Uses `Materials/EmissiveGlow.material` (warm emissive, intensity 5) — bloom check |
| **Depth Column** | Tall cylinder — shadow / AO contrast |

## Open in editor

1. Create or open a 3D project (Empty 3D template is fine).
2. Copy this folder into the project `Assets/` tree (keep `Materials/` and `Scenes/` layout), **or** symlink `assets/validation/graphics_validation` into the project.
3. **File → Open Scene** → `Scenes/GraphicsValidation.scene`.
4. If the emissive sphere looks flat gray, select **Emissive Sphere** → Inspector → Material slot → pick **EmissiveGlow** (asset DB may need a refresh after copy).

### One-time manual checks (serializer gaps)

- **Fill Spot** → Inspector → enable **Cast Shadows** (local shadow map smoke).
- Confirm **Environment** entity has **Primary** checked (authored in scene; verify in Inspector).

## Capture procedure

Defaults (no `graphics.json` overrides): Scene View = **Low**, Game View = **High**, Play = **High**.

1. Frame both Scene and Game views on the cluster (camera at `(0, 1.5, 8)`).
2. **Scene View** — ensure quality badge shows **Low** → screenshot → `scene_low.png`.
3. **Game View** (not playing) — badge **High** → `game_high_edit.png`.
4. Press **Play** — Game View still **High**, no editor gizmo overlay in Game → `game_high_play.png`.
5. Optional: **View → Rendering → Render Diagnostics** — copy Logical/Internal resolution, draws, meshes, CPU/GPU ms into `RESULTS.md`.

Screenshot helper (Windows):

Save comparison captures under `artifacts/graphics-validation/`; that directory is intentionally excluded from Git.

Save all three PNGs under `artifacts/graphics-validation/`.

## Acceptance cues

- Low Scene View stays interactive while editing.
- High Game View shows stronger shadows, SSAO, bloom on emissive sphere, and AA vs Low Scene.
- White cube albedo stays neutral white under both presets (no surprise tint shift).
