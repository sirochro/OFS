# tools — sirochro's OFS bulk-edit extension

Adds a panel for bulk-editing the **current selection** of the active funscript.

## Install

Copy this folder into your OFS extensions directory:

```
Windows: %AppData%\OFS\OFS3_data\extensions\tools\
```

Then in OFS:

1. Restart OFS (or reload extensions)
2. Menu → **Extensions** → enable **tools**
3. Menu → **Extensions** → **tools** to show the panel

## Panels

### Set position

Force every action in the current selection to a chosen `pos` (0–100).

- **Slider / input** — pick an absolute value
- **-10 / +10** — nudge the value without applying
- **All Btm / All Mid / All Top** — apply 0 / 50 / 100 immediately
- **Apply** — commit the slider value

### Optimize waves

Detect "waves" (significant peaks and valleys) in the selection and reshape
them. Wobbles whose amplitude is below the tolerance are absorbed into the
dominant direction.

- **Tolerance** — 0..100. Higher = more aggressive smoothing.
- **Preset**:
  - **None (wobble removal only)** — keep significant extrema as-is
  - **0-100 (force endpoints)** — normalize each kept extremum to 0 or 100
  - **QUATRO (4-point quartile)** — replace each 3+-point wave with four
    time-evenly-spaced points using a quartile profile that depends on
    direction and the second sample (≥50 vs <50):

    | direction | 2nd point | output |
    |-----------|-----------|--------|
    | ascending | ≥50       | 0, 75, 55, 100 |
    | ascending | <50       | 0, 25, 45, 100 |
    | descending| ≥50       | 100, 75, 55, 0 |
    | descending| <50       | 100, 25, 45, 0 |

    Two-point waves are normalized to 0/100 without inserting wobble.

All actions taken via this extension are wrapped in a Lua-script undo entry
(`ofs.Undo()` reverts the last Lua-driven change).
