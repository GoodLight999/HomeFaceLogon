# Sign-in option icon

`scan-face.svg` comes from [Lucide](https://github.com/lucide-icons/lucide/blob/main/icons/scan-face.svg)
and is covered by `LICENSE-lucide.txt`. The installer includes that notice.

LogonUI requires a bitmap through `GetBitmapValue`, rather than an SVG. The checked-in
`../tileimage.bmp` is a 72x72, 32-bit, premultiplied BGRA resource with transparent
padding and a white glyph. `LoadImageW(..., LR_CREATEDIBSECTION)` preserves its pixels.
The separate provider label identifies it as face authentication.

To regenerate using an already installed Sharp module:

```text
node tools/render-provider-icon.mjs /path/to/sharp/dist/index.mjs
```

The converter also writes ignored previews at 32 and 72 pixels under `local/icon-preview`.
Normal builds use the committed bitmap and need no new image-processing dependency.
