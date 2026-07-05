# Netuoso Animator

Keyframe animation tool for the level editor — like in Blender or After Effects.

## How it works

- A **timeline panel** is docked at the bottom of the editor. Drag the playhead or use the step buttons to move through time.
- Select an object and press **Key** (or the <cy>I</cy> key) to place a keyframe at the current time. The object gets a free group ID automatically.
- Enable **Record** — every move / rotate / scale of the selected object drops a keyframe automatically.
- Keyframes show up as <cy>diamonds</cy> on the timeline for the selected object.
- Press **Gen** — the mod resets each object to its first keyframe and spawns real <cg>Move</cg> / <cg>Rotate</cg> / <cg>Scale</cg> triggers between every pair of keys. The level works without the mod installed.
- **Trash** clears all keyframes of the selected object (with confirmation).
- The panel collapses into a thin strip via the chevron button.

Keyframes are stored per level in the mod's save folder, so you can come back and edit the animation later — nothing touches the level until you press Gen.

## Settings

- **Move units multiplier** — calibration for Move trigger offsets.
- **Playhead step** — seconds per prev/next click.
