# Hardware

KiCad design files, schematic PDFs and fabrication files for the RIPbox badge.

| Folder | Board | Status |
|---|---|---|
| [`supercon-v1.0/`](supercon-v1.0) | The Hackaday Supercon 2026 badge | Released, has [3 known issues](supercon-v1.0/KNOWN_ISSUES.md) |
| [`v1.1/`](v1.1) | Corrected revision | Files coming soon |

Each version folder has the same layout:

| Subfolder | Contents |
|---|---|
| `kicad/` | The full KiCad project (schematic, PCB, libraries). Open the `.kicad_pro` file. |
| `schematics/` | PDF exports of the schematic, for viewing without KiCad |
| `gerbers/` | Gerber and drill files, ready to upload to a PCB fab |

## Opening the design

1. Install [KiCad](https://www.kicad.org/).
2. Open the `.kicad_pro` file inside the `kicad/` folder of the version you want.

## Ordering a board

1. Zip the contents of the `gerbers/` folder (the Gerber and drill files, not the folder itself).
2. Upload the zip to your PCB fab of choice.
3. Use the board settings noted in the folder (once added), or check the PCB layout in KiCad for the stackup and thickness.

If you want a working board today, use **v1.1** once it's published. If you're building **Supercon v1.0**, read the [known issues](supercon-v1.0/KNOWN_ISSUES.md) first.

## SAO connector

6-pin SAO: **1** VCC · **2** GND · **3** SDA · **4** SCL · **5** GPIO1 · **6** GPIO2. See the [main README](../README.md#sao-connector-pinout) for the diagram.
