# The Libreworks document format (`.lwd`), version 3

An `.lwd` file is a ZIP archive (stored or deflated entries; no encryption or ZIP64) containing these files:

| Entry | Contents |
|---|---|
| `mimetype` | First entry, stored uncompressed: `application/x-libreworks` (this follows the ODF/EPUB convention, so `file` can identify the archive). |
| `document.json` | The document: canvas, layers, frames, objects, export settings. UTF-8 JSON. |
| `preview.png` | A flattened render of frame 1, at most 256×256, for file managers and other tools. Readers ignore it. |
| `images/<id>.png` | One 8-bit RGBA PNG per bitmap object (not premultiplied). |

Readers must reject documents whose `version` is newer than they understand.
They should ignore unknown keys, so that additive changes do not need a version bump.

## `document.json`

```json
{
  "format": "libreworks-document",
  "version": 3,
  "generator": "Libreworks 0.2.0",
  "width": 640, "height": 420,
  "canvasColor": "#ffffffff",
  "loops": 0,
  "currentFrame": 0, "currentLayer": 1,
  "layers": [ { "id": "1", "name": "Background", "visible": true, "locked": false }, ... ],
  "frames": [
    { "name": "Frame 1", "delay": 100, "layers": [ [ <object>, ... ], [ ... ], ... ] },
    ...
  ],
  "symbols": [ <symbol>, ... ],
  "export": { "format": "png", "quality": 80, "lossless": false, "colors": 256, "palette": 0,
              "dither": true, "animated": false, "netpbmAscii": false, "matte": "#ffffffff" }
}
```

* Colours are `#AARRGGBB` strings.
* Coordinates are canvas pixels as floating-point numbers, with the origin at the top left.
* Rectangles are `[x, y, w, h]` and points are `[x, y]`.
* `layers` lists the layers bottom to top. A layer may also have `"shared": true` (Share Across
  Frames: every frame keeps the same objects on it) or `"web": true` (the Web Layer, which holds
  slices and hotspots). All frames share the layers, so every frame has the same number of layers.
* `frames[i].layers[j]` is the list of objects on layer `j` in frame `i`, bottom to top.
* `delay` is in milliseconds.
* `loops` is the animation repeat count; `0` means forever.
* `export.format` is one of: `png`, `apng`, `jpeg`, `webp`, `gif`, `avif`, `jxl`, `bmp`, `tiff`, `ppm`, `pgm`, `pbm`.
* `export.palette`: 0 adaptive, 1 web 216, 2 grayscale, 3 black & white.
* `export.netpbmAscii` chooses P3/P2 (`true`) or P6/P5 (`false`).
* `previewPanes` (optional) holds 3 `export` objects: the settings of the three optimised 2-Up/4-Up
  preview panes. Each pane owns its settings. `selectedPane` (0–2) names the selected pane, and
  `export` always equals `previewPanes[selectedPane]`. Both keys are written only after a 4-Up view
  has been opened.
* Ids are 64-bit integers written as decimal strings.

### Objects

Every object has these keys:

| Key | Type | Notes |
|---|---|---|
| `id` | string | Unique within the document. |
| `type` | string | `bitmap`, `rect`, `ellipse`, `line`, `path`, `text`, `slice`, `hotspot`, `instance`. |
| `name` | string | Can be empty. |
| `opacity` | int | 0–100. |
| `blend` | string | `normal`, `multiply`, `screen`, `overlay`, `darken`, `lighten`, `difference`, `colorDodge`, `colorBurn`, `hardLight`, `softLight`, `exclusion`. |
| `visible`, `locked` | bool | |
| `effects` | array | Optional; see the Live Effects section. |

Keys that depend on the type:

* **bitmap**:
  * `rect`: where the image is placed. The image is scaled to fit the rect if the sizes differ.
  * `image`: an entry path such as `images/12.png`.
* **rect**, **ellipse**: `rect`. A rect may also have `roundness`, from 0 to 100: the corner radius as a percentage of half the shorter side.
* **line**: `p1`, `p2`.
* **path**: `points` (every anchor point, all contours in order) and `closed` (bool, applies to
  every contour). Optional since version 2:
  * `handlesIn` / `handlesOut`: arrays the same length as `points` holding each anchor's absolute
    Bezier control points (the handle on the incoming and the outgoing segment). A handle equal to its
    anchor is a corner. The segment from anchor *i* to *i+1* is the cubic
    `points[i], handlesOut[i], handlesIn[i+1], points[i+1]`; a closed contour also has the segment
    from its last anchor back to its first. Without these keys every segment is straight.
  * `contours`: increasing start indices of the 2nd, 3rd... contour of a compound path (Join,
    Punch with a hole, text converted to paths). The area is filled with the even-odd rule.
* **text**: `rect`, `text`, `font` (family), `fontSize` (pixels), `bold`, `italic`, and `align` (0 left, 1 centre, 2 right).
  The text is drawn with the fill colour.
  `textAntialias` is one of `none`, `crisp`, `strong`, `smooth` or `system` (the Fireworks MX 2004 modes).
  It was added in 0.2. When it is missing, readers use `smooth` if `antialias` is true and `none` otherwise.
  Since 0.2 a text block may have `onPath`: `{ "points", "closed", "handlesIn", "handlesOut",
  "contours", "offset" }` (same keys as a path, plus the start offset in pixels along it). The text
  is then laid out along that guide path instead of in its `rect` (Text > Attach to Path).
* **slice**, **hotspot** (version 2; they live on the layer marked `"web": true`):
  `rect`, `link`, `alt`, `target`, `behavior` (`none`, `simpleRollover`, `swapImage`),
  `swapTarget` (an object id, optional) and `swapFrame`. A hotspot also has `shape` (`rect`,
  `circle`, `polygon`); polygons store `points`. A slice may have `optimize`, an `export` object
  with its own settings.
* **instance** (version 2): a placed copy of a symbol. `rect` is where the symbol's bounds are
  mapped to (so it also stores the scale), `symbol` is the symbol id and `symbolFrame` is the
  symbol frame (for buttons the state: 0 Up, 1 Over, 2 Down, 3 Over While Down) it shows.
  Button instances may have `link`, `alt` and `target`. Instances can carry `effects` and `opacity`.
* **group** (version 3, Libreworks 0.3.2): `children`, an array of objects (bottom first, any
  type, groups nest). A group has no `rect`, `fill` or `stroke`: its bounds are its children's.
  `opacity`, `blend` and `effects` apply to the rendered group as a unit. Optional `mask`:
  `{ "object": <object>, "mode": "outline" | "grayscale", "enabled": bool, "linked": bool }`.
  `outline` masks by a vector object's path area or a bitmap's alpha; `grayscale` by the mask's
  luminance times its alpha (white shows, black hides). Nothing shows outside the mask object.
  A linked mask moves and transforms with the group.
* Every vector object also has:
  * `fill`: `{ "on": bool, "color": "#AARRGGBB" }`. Optional keys added in 0.2:
    * `kind`: `solid` (default), `webDither`, `pattern`, or a gradient: `linear`, `radial`, `cone`,
      `contour`, `satin`, `starburst`, `bars`, `ripples`, `waves`, `ellipse`, `rectangle`.
    * `gradient`: `[ { "pos": 0..1, "color": "#AARRGGBB" }, ... ]`. When missing, gradients run
      from the fill colour to white.
    * `handles`: three points `[start, end, axis2]`, **normalised to the object's bounds**
      (0,0 = top-left, 1,1 = bottom-right), so the gradient follows moves and scaling.
      `end` sets the gradient direction and length; `axis2` is the second axis of elliptical kinds.
      When missing, readers use the defaults (left-to-right through the centre).
    * `pattern`: a built-in pattern name (`Berber`, `Bricks`, `Checkerboard`, ...), or `Custom`
      with `patternImage`, a PNG inside the archive (`images/pattern<id>.png`).
    * `texture` (a built-in texture name) and `textureAmount` (0..100 %).
    * `feather`: px. A positive value means a feathered edge; otherwise `antialias` chooses
      between the Anti-Alias and Hard edges.
  * `stroke`: `{ "on": bool, "color": "#AARRGGBB", "width": px }`. Optional since 0.2: `kind`, a
    stroke category key such as `pencil.1pxHard`, `charcoal.soft`, `airbrush.basic`, `dashed.basic`
    (`basic.hard` when missing), and `align`: `inside` or `outside` (centred on the path when missing).
  * `antialias`: bool.

### Symbols

`symbols` (version 2, optional) is the document Library:

```json
{ "id": "42", "name": "Nav Button", "kind": "button",
  "frames": [ [ <object>, ... ], [ ... ], [ ... ], [ ... ] ] }
```

* `kind` is `graphic` (1 frame), `animation` (1 or more frames) or `button` (4 frames: Up, Over,
  Down, Over While Down).
* Each frame is a list of objects in symbol coordinates (the symbol's own bounds are the union of
  its frames). Objects may be any vector or bitmap type, or nested instances.
* Bitmaps inside symbols are stored as `images/sym<symbolId>_<frame>_<objectId>.png`.
* Symbol ids and object ids inside symbols share the document's id space.

### Live Effects

`effects` is the object's editable effect stack, applied in list order (the Property Inspector's
Effects list, top to bottom). Every effect has the common keys:

```json
{ "type": "dropShadow", "enabled": true, "distance": 7, "angle": 315, "softness": 4,
  "color": "#ff000000", "opacity": 65, "width": 4 }
```

* `enabled: false` keeps the effect in the stack but skips it (the checkbox in the list).
* `angle` is in degrees, counter-clockwise, with 0 pointing right. For shadows it is the offset
  direction; for bevels and embosses it is the direction the light comes from (135 = top-left);
  for Motion Blur it is the blur direction.
* `distance` is in pixels (shadow offset, Motion Blur length).
* `softness` is a blur radius in pixels (shadows, glows, bevel softness, Gaussian Blur radius,
  Unsharp Mask radius). `opacity` runs from 0 to 100.
* `width` is the glow width or the bevel/emboss size, depending on the effect.
* Unknown keys are ignored and missing keys take the defaults listed below, so older files load.

Rendering: pixel adjustments (Adjust Color, Blur, Sharpen, Noise, Other) are applied to the object
in list order first; then Drop Shadow, Glow and Outer Bevel are drawn beneath the object, and
Inner Shadow, Inner Glow, Inner Bevel and the embosses on top of it.

| `type` | Category | Extra keys (default) |
|---|---|---|
| `autoLevels` | Adjust Color | none; computed from the image (0.5 % clip per channel) |
| `brightnessContrast` | Adjust Color | `brightness` −100..100 (0), `contrast` −100..100 (0) |
| `colorBalance` | Adjust Color | `balance`: 3×3 array `[[cr, mg, yb] shadows, midtones, highlights]`, each −100..100 (0); `preserveLuminosity` (true) |
| `colorFill` | Adjust Color | `color`, `opacity` (50), `blend` blend-mode key (`normal`) |
| `curves` | Adjust Color | `curves`: 4 arrays (RGB, Red, Green, Blue) of `[x, y]` points in 0..255, interpolated with a monotone cubic; an empty array is the identity. Channel curves apply first, then the RGB curve. |
| `desaturate` | Adjust Color | none |
| `hueSaturation` | Adjust Color | `hue` −180..180 (0), or 0..360 when `colorize`; `saturation` −100..100 (0; 0..100 when colorizing); `lightness` −100..100 (0); `colorize` (false) |
| `invert` | Adjust Color | none |
| `levels` | Adjust Color | `levels`: 4 arrays (RGB, Red, Green, Blue) of `[inBlack, inWhite, outBlack, outWhite, gamma]` (default `[0, 255, 0, 255, 1]`). Channel levels apply first, then RGB. |
| `photoFilter` | Adjust Color | `color` (#EC8A00 warming 85), `amount` density 0..100 (25), `preserveLuminosity` (true) |
| `posterize` | Adjust Color | `posterize` levels per channel, 2..255 (4) |
| `threshold` | Adjust Color | `threshold` 1..255 (128); luminance at or above it becomes white |
| `innerBevel`, `outerBevel` | Bevel and Emboss | `width`, `softness`, `angle` (135), `opacity` = contrast, `color` (outer bevel colour), `preset` 0 Raised, 1 Highlighted, 2 Inset, 3 Inverted |
| `insetEmboss`, `raisedEmboss` | Bevel and Emboss | `width` (5), `softness`, `angle` (135), `opacity` = contrast, `showObject` (true) |
| `blur`, `blurMore` | Blur | none (fixed 3×3 / 5×5 box) |
| `gaussianBlur` | Blur | `softness` = radius |
| `motionBlur` | Blur | `angle` (0), `distance` (10) |
| `radialBlur` | Blur | `amount` spin strength (10), `quality` samples (20) |
| `zoomBlur` | Blur | `amount` (20), `quality` samples (20); centred on the object |
| `convertToAlpha` | Other | none; luminance becomes transparency (black opaque, white transparent) |
| `findEdges` | Other | none |
| `noise` | Noise | `amount` 0..100 (15), `colorNoise` (false); deterministic (seeded by pixel position) |
| `dropShadow`, `innerShadow` | Shadow and Glow | `distance`, `angle`, `softness`, `color`, `opacity` |
| `outerGlow`, `innerGlow` | Shadow and Glow | `width`, `softness`, `color`, `opacity` |
| `sharpen`, `sharpenMore` | Sharpen | none |
| `unsharpMask` | Sharpen | `amount` 1..500 % (50), `softness` radius (1), `threshold` 0..255 (0) |

The Filters menu applies the same adjustments destructively to bitmap objects (clipped to the
pixel selection when there is one); nothing is stored for those.

Version-1 files have no `preset` on `innerBevel`; such bevels load with `angle` 135, which reproduces
the 0.1 look.

## Compatibility

* Version 1 is the format written by Libreworks 0.1.
* Version 2 (Libreworks 0.2) adds slices, hotspots, web/shared layers, symbols and instances,
  `textAntialias`, and the extended Live Effects. Version-1 files load unchanged.
* Version 3 (Libreworks 0.3.2) adds `group` objects and masks. Version-1 and -2 files load unchanged.
* Future versions only add keys where they can; any change to existing semantics increments `version`.

## User styles file (`styles.json`)

Styles saved from the Styles panel (New Style) are stored outside documents in
`$XDG_CONFIG_HOME/libreworks/styles.json` (default `~/.config/libreworks/styles.json`).
Built-in presets are compiled into the application and are never written to this file.

```json
{
  "format": "libreworks-styles",
  "version": 1,
  "styles": [
    {
      "name": "Green Thing",
      "fill": true, "stroke": false, "effects": true, "text": false,
      "attributes": { "type": "rect", "fill": "#ff0ac81e", "effects": [ ... ] }
    }
  ]
}
```

* `fill` / `stroke` / `effects` / `text` say which attribute groups the style carries
  (the checkboxes of the New Style dialog). Applying a style only touches those groups;
  geometry and text content are never changed. Fill and stroke are not applied to bitmaps,
  text attributes only to text objects, and nothing is applied to slices or hotspots.
* `attributes` is an object in exactly the document object format above (the same keys as an
  entry of a layer's `objects` array, including the `effects` array), so new fill/stroke
  fields automatically round-trip through styles too.
