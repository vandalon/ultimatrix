# Ultimatrix enclosure prototype

Parametric OpenSCAD design for one 64×64 HUB75 panel and an internal Matrix Portal S3. This is a mechanical starting point, not a fit-verified production enclosure.

| Variant | Panel | Shell outside | Shell depth | Diffuser cut size |
|---|---|---|---|---|
| P3 | 192 × 192 × 14 mm | 204 × 204 mm | 32.8 mm | 198.4 × 198.4 mm |
| P2.5 | assumed 160 × 160 × 14 mm | 172 × 172 mm | 32.8 mm | 166.4 × 166.4 mm |

The 2.4 mm rear cover is recessed, so total depth is 32.8 mm. The shell has 6 mm outer corner radii, a 1 mm front chamfer and a 0.3 mm gap around the flush rear cover. The visible front border is about 5 mm wide along each straight edge. Sides are solid; ventilation is in the rear only. Rear clearance is adjustable with `rear_clearance`, initially 12 mm. Controller, plugs and cable bends still need measuring before a full print. P2.5 panel thickness is unverified. Published P3 dimensions and M3 threads: https://www.tinytronics.nl/nl/verlichting/matrix/cailiang-64x64-rgb-led-matrix-192x192mm

## Front stack

The front rim retains a 2.6 mm black LED diffusion acrylic sheet, which also serves as the exposed protective surface. No extra window. Fit a thin perimeter gasket or spacer to prevent rattle. The removable black grid sits behind it; the panel supports the grid from the rear. No glue on the optical surface.

The grid uses 0.4 mm walls and 3 mm cells for P3, or 2.5 mm cells for P2.5. The diffuser is initially 3 mm above the LED tops. Export the 8×8 sample at gaps of 2, 3 and 4 mm before choosing a full grid. A thick diffuser can spread light within its thickness; square pixels and zero bleed are not guaranteed.

The grid stops at LED-top height. It needs an underside skirt reaching between LED packages to block leakage below the cells. That skirt is deliberately pending actual LED package size/height and panel mask measurements. Do not press the grid onto LEDs. A 64-cell grid also needs alignment checked across its entire width.

## Printing and assembly

- Shell: front face on bed, black PETG, 0.4 mm nozzle, 0.2 mm layers, 3 walls. Check the shallow diffuser-pocket overhang in the slicer; local support may be needed. Main cavity corners have no unsupported roof.
- Rear: outside face on bed, PETG, 0.2 mm layers. Four M3 screws enter the shell's pilot holes. Select length after checking the actual lid and screw engagement; usable pilot depth below the recessed lid is about 6.6 mm. Do not overtighten.
- Grid/sample: flat on bed, opaque black material, preferably 0.2 mm nozzle. Confirm every wall is present in the slicer. No translucent infill or white walls.
- Diffuser loads from the rear before the grid/panel. Nominal pocket clearance is 0.2 mm per side; check the supplier's actual sheet thickness before cutting.
- Keep the 5 V power supply external. The cable notch is provisional and needs a strain-relief clamp sized to the chosen cable.

## Still needed before a full print

Panel retention brackets are not yet modelled: obtain rear mounting-hole coordinates, usable M3 thread depth, and rear component heights. Mounting hardware must hold the panel independently of its LED packages. Rear slots are generic fixture locations, not verified Matrix Portal mounts. The controller needs a measured bracket, USB/button access and antenna clearance. Wall keyholes need appropriate screw heads and spacing from the wall for airflow. Desk stand and cable clamp remain to be designed.

Exported parts are prototypes for slicing and inspection. Print only the optical samples until retention and controller clearances are resolved.

## Export

Open `enclosure.scad` in OpenSCAD. Set `pitch` to 3 or 2.5 and `part` to `shell`, `rear`, `grid`, or `sample`. Use F6 then Export STL. `assembly` shows an exploded rear lid.

Example: `openscad -o p3-sample.stl -D 'pitch=3' -D 'part="sample"' enclosure.scad`
