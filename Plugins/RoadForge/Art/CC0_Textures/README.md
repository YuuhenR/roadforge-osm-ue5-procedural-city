# RoadForge CC0 detail textures

Photo-scanned PBR surface textures used to give the procedural roads real surface detail
(so Lumen reflections / Virtual Shadow Maps have micro-relief to work with, instead of a flat slab).

## Source & licence

Downloaded from **[ambientCG](https://ambientcg.com)** — released into the **public domain (CC0 1.0)**.
No attribution is required; you may use, modify and redistribute them freely.

| File | ambientCG asset | Channel |
|---|---|---|
| `T_RF_Asphalt_BC.png` | Asphalt026C (2K) | Base colour (sRGB) |
| `T_RF_Asphalt_N.png`  | Asphalt026C (2K) | Normal (DirectX / UE convention) |
| `T_RF_Asphalt_R.png`  | Asphalt026C (2K) | Roughness (linear) |
| `T_RF_Asphalt_AO.png` | Asphalt026C (2K) | Ambient occlusion (linear, unused by default) |
| `T_RF_Concrete_BC/N/R.png` | Concrete016 (2K) | Spare set for a future sidewalk/building split |

## How to wire them in (one time, ~30 s)

1. In the editor, Content Browser ▸ gear icon ▸ enable **Show Plugin Content**.
2. Under **RoadForge ▸ Content**, make a folder named exactly **`Textures`**.
3. Drag the four `T_RF_Asphalt_*.png` files from this folder into that `Textures` folder
   (or right-click ▸ Import). Keep the names — the material looks for `/RoadForge/Textures/T_RF_Asphalt_N` etc.
4. Run **Tools ▸ RoadForge ▸ Regenerate RoadForge Material**.

That's it. The material's build code auto-fixes the import settings (Normal → normal-map, Roughness → linear),
so you don't have to touch sRGB / compression by hand. If the textures are missing, the material falls back to
a procedural noise version that still looks far better than the old flat one — so this step is optional.
