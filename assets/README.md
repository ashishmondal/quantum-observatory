# Image assets

Drop **64×32, 8-bit indexed, uncompressed** `.bmp` files in this folder.
On the next `pio run` the pre-build step ([../tools/bmp_to_header.py](../tools/bmp_to_header.py))
auto-generates `include/bitmaps/<name>.h` containing an
`inline constexpr uint16_t k<Name>[64 * 32]` array of RGB565 values
(panel-native), plus an `_index.h` registry so the firmware can find them.

Show one over MQTT:

```text
observatory/scene  {"scene_id":"bg_image","priority":3,"sticky":true}
```

Today the `bg_image` scene always shows the **first** registered image.
A `image_id` selector can be added later when there's more than one.

## Authoring tips

- Most tools save 24-bit by default. In **GIMP**: Image → Mode → Indexed
  (max 256 colors), then Export As `.bmp`, advanced options → 8 bpp.
- In **Photoshop**: Image → Mode → Indexed Color, Save As BMP, depth 8-bit.
- The exact dimensions matter — the converter rejects anything that isn't
  64×32 to keep the firmware path branch-free. Resize before export.
- Compression must be off (`BI_RGB`). Most tools do this by default for
  8-bit BMPs.
