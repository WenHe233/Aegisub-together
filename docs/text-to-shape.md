# Turning text into shapes

How Muteki Aegisub converts a line of text into a drawing that renders identically, what the
renderers really do that has to be reproduced, and the two things that most often go wrong: the
handling of `\b1` / `\i1`, and the seams between clipped bands.

Written for anyone continuing the work. Everything here was arrived at by reading the vendored libass
source (`subprojects/libass/libass/`) and by measuring, not by guessing; where something is knowingly
approximate it says so.

Code: `src/text_to_shape.cpp` (measuring, conversion, splitting, decorations),
`src/typesetting_transform.cpp` (mapping shapes, clips, the bending modes),
`src/visual_tool_transform.cpp` (the visual tools that drive them).

---

## 1. What a converted line looks like

One piece of text becomes:

```
{<the tags the line carried>\an7\pos(X,Y)\fscx100\fscy100\frz0\p1}m … l …{\p0}
```

* **`\an7\pos(X,Y)`** — `X,Y` is the point the line's own alignment anchored, i.e. the value of its
  `\pos` (or where the margins and the alignment put it, when it has none). The drawing's
  coordinates are written relative to that point.

  This works because of how libass places a drawing: for `OUTLINE_DRAWING` it sets
  `val->asc = bbox height`, `desc = 64 * \pbo`, and `offset.y = -asc * scale.y`, with `offset.x = 0`
  (`ass_render.c`, `ass_outline_construct` and the block above it). The baseline therefore sits
  exactly `asc` below the top of the glyph box, and the offset lifts it back by the same amount — so
  **drawing coordinate (0,0) lands on the `\an7` anchor whatever the bounding box is**, and `\pbo`
  cancels out.

* **`\fscx100\fscy100\frz0`** — the scale and the turn are multiplied into the coordinates, so they
  must be neutralised or they would be applied twice. Taking the tags out is not enough, because a
  style can also say them.

* **`\p1`** — coordinates are written in whole script units; `\p<n>` would divide them.

* Tags that are *swallowed* by the conversion must never be rewritten: `IsBakedTag()` lists them
  (`\fn \fs \b \i \u \s \fsp \fscx \fscy \frz \fr \org \frx \fry \fax \fay \pos \move \an \a`).
  Everything else the line carried — colours, `\blur`, `\be`, fades, `\clip`, `\t` — is copied over
  untouched.

* **`\clip` is deliberately left alone.** A clip is given in script coordinates and means the same
  thing to a drawing as to text.

A line is broken into **pieces**: one per stretch of text with its own font, size, scale, lean or
turn, and one per row. They are all measured together so they share the rows and the baselines, and
each piece becomes a drawing of its own with the same `\pos`.

---

## 2. Measuring with GDI so that libass agrees

* **Font size.** Ask GDI for a **positive** `lfHeight`, which is the *cell* height. That is what
  libass gets from `FT_SIZE_REQUEST_TYPE_REAL_DIM` with `face->ascender/descender` taken from
  `usWinAscent/usWinDescent`. Verified to 1.0000–1.0001 across two dozen fonts. Asking for a
  character height instead gives a noticeably bigger font.
* **`UPSCALE = 64`.** The font is created 64× larger and the path divided back down, so GDI's
  integer path coordinates still carry sub-pixel detail.
* **Advances, one character at a time.** `GetTextExtentPoint32W` per character, fed back to
  `ExtTextOutW` as an explicit advance array. Asking GDI for the extent of a whole run lets it adjust
  the run, and that difference grows along the row. Measured: per-character GDI advances equal
  OpenType-shaped advances with libass's feature set (liga/clig on, **kerning off** by default —
  `track->Kerning`).
* **`\fsp`** goes into the advances by hand, with `SetTextCharacterExtra(dc, 0)`, or it would be
  counted twice.
* **Outlines** come from `BeginPath` / `ExtTextOutW` / `EndPath` / `GetPath`.
* **Underline and strikeout** are *not* asked of GDI (`lfUnderline = FALSE`). libass lays a bar under
  **each glyph** in the width of that glyph's own advance **excluding `\fsp`**
  (`ass_outline_add_rect(outline, 0, line_y, adv, …)`), which is why the line looks dashed when
  letters are spaced out. Bars are emitted per glyph, wound the same way as the glyph outline so the
  non-zero rule unions them instead of cutting a gap. Position and thickness come from
  `GetOutlineTextMetricsW` (`otmsUnderscorePosition/Size`, `otmsStrikeoutPosition/Size`), centred on
  the position the font names, and only where the font names one at all.

### Layout rules worth knowing

* Trimming: libass treats only `' '` and `'\n'` as whitespace (`IS_WHITESPACE`). `\h` becomes NBSP
  and is **never** trimmed; a tab becomes a space; `\n` becomes a space unless `wrap_style == 2`.
* A row that held nothing at all is **half** a line high; a row of spaces is full height. Trimmed
  whitespace does not contribute metrics unless the row is otherwise empty.
* Rows share a baseline, so a piece in a smaller font sits lower.
* **Shear pivots.** `\fax` shears each glyph about **its own ascent line**
  (`x1[3] = {1, fax, info->shift.x + info->asc * fax}`), and `\fay`'s accumulated baseline shear
  **resets at every style run** (`apply_baseline_shear`). This is why a word in another size slides
  out of a leaning row, and why `SplitForShear()` puts each stretch on a line of its own anchored at
  its own corner (its ascent line and its first letter) — the one point of it a lean leaves alone.
* Drawings fill by the **non-zero winding** rule (`get_fill_flags`: `winding ? FLAG_SOLID : 0`,
  `default: FLAG_SOLID`).
* A border is stroked with a pen of `bord / scale`, so it comes out the width that was asked for
  whatever the scale, and it is **sheared and turned with the letters**.
* A shadow's offset is added to `info->shift`, which rides in the translation column of the
  transform: it is **turned** by `\frz` (and by the 3D chain) but never scaled or sheared.

---

## 3. `\b1` — the bold trap

**The problem.** GDI answers questions about weight with the weight you *asked for*, not the weight
it found. If the family has no bold vagate, GDI invents one — and its invented bold **walks the
letters further apart**. The renderers do not: they thicken the outline and leave the advances alone.
Converting with GDI's fake bold therefore produced a shape that was too wide, and the error grew
along the row.

**How the renderers decide.** libass synthesises a bold when what it found is more than a step and a
half short of what was asked for:

```c
desc.bold > ass_face_get_weight(face) + 150
```

and the thickening itself is `ass_glyph_embolden` → `FT_Outline_Embolden`, described in the source as
emboldening the glyph *"without touching its metrics"*.

**What this code does.** Read the font file itself rather than asking GDI:

```c
GetFontData(dc, 0x322F534F /* 'OS/2' */, 0, header, sizeof(header));
weight    = big-endian u16 at offset 4   // usWeightClass
selection = big-endian u16 at offset 62  // fsSelection  (bit 0 = italic, bit 5 = bold)
```

`FaceWeight()` maps the old one-to-nine scale onto real weights, and a zero means the font did not
answer, in which case the style bit is all there is to go on. Then:

* If `700 > FaceWeight + 150`, GDI would be inventing the bold. **Measure and draw with the real
  (non-bold) face.** That is far closer to what the renderer draws than GDI's invention: libass
  pushes the outline out by about a sixty-fourth of the em — roughly a pixel and a half at size 120,
  and nothing at all beside GDI's fake bold.
* If the face really is bold, use it as asked.

**Known gap.** libass's own ~1/64-em embolden is *not* replicated, so a synthesised bold comes out
about a pixel thinner per side than the renderer would draw it. If that ever matters, dilate the
outline by `em/64` (the widening helper described in §5 already does exactly this kind of dilation).

---

## 4. `\i1` — the opposite rule

**Do not take the italic off.** This is the mistake to avoid, and it was made once here: the fake
bold and the fake italic were dropped together, which stood the letters back up and made `\i1`
produce a shape that was not italic at all — for *every* ordinary font, since ordinary fonts are
exactly the ones without an italic vagate.

The two cases are not symmetric:

| | who invents it | what it changes | so |
|---|---|---|---|
| `\b1` | GDI **and** libass | GDI's fake bold changes **advances** | drop it, use the real face |
| `\i1` | GDI **and** libass | a slant changes **no advance** | keep it |

libass makes an italic up by slanting the outline; a shear leaves every advance alone. So the italic
is kept exactly as it was asked for, and the letters can be measured with it.

**Style values count too.** `Italic` in the style arrives by the same road as `\i1`: `StyleOnly()`
seeds the running state from the style (`font, fontsize, bold, italic, underline, strikeout, spacing,
alignment, scalex, scaley, angle`, plus `outline_w`/`shadow_w`), and `ApplyTag()` then applies the
line's own tags on top. Anything the style can say has to be seeded there, or a line that says
nothing will convert wrong. Do not add tag reading without adding the style seed beside it.

**Known gap.** GDI's synthetic-italic slant is not guaranteed to equal libass's fixed shear. If a
converted italic ever visibly disagrees with the text it replaced, the fix is to drop `lfItalic`,
measure upright, and apply libass's own shear to the outline yourself — not to drop the italic and
leave it there.

---

## 5. Border and shadow as shapes

`\bord` is one number for an **upright** pen, and it cannot be negative. So once a line has been
sheared, narrowed or bent, there is no border value that follows its letters — no tag-level rule can
be exact. The answer is to make the pair part of the same geometry:

* The **border** is the Minkowski sum of the shape and the pen: the shape's own rings, plus a band
  along every edge, plus the pen's shape at every corner that turns away from the ink. Everything
  added is wound the way the shape is wound, so the non-zero rule unions it — a hole stays a hole
  (the band that cancels its winding reaches only as far as the pen does) and the middle stays solid,
  so no seam shows where a band meets the letter. The shape is kept **inside** its own border, which
  is what keeps the middle solid.
* The pen is `(xbord, ybord)` in the frame the shape is written in: script units for a converted
  shape (`\fscx100`), and `bord / (scale/100)` in a glyph frame that still carries `\fscx`.
* The **shadow** is that bordered shape again, moved by the offset. Because the offset rides in the
  translation column, it is turned by `\frz` and by nothing else — so it is turned exactly when the
  turn has been taken out of the coordinates.
* Order matters: **shadow, border, fill**. The fill is left out altogether when it is painted the
  border's own colour, because the bordered shape already holds it.
* Comparing those colours: read `\1c` **and** `\c` (the same tag), fall back to the style, compare
  **RGB only** — a colour off a tag carries no alpha, one off a style carries the style's — and
  compare the alpha separately as a **byte**, since `&H00&`, `&h00&` and a style's 0 are one value.

Helpers: `text_to_shape::WidenRings()`, `ReadDecorations()`, `BakeDecorations()`.

---

## 6. Clip seams — why bands stripe, and the fix

**The symptom.** A gradient is typically a stack of copies of the same line, each clipped to one
band. After conversion or bending, thin stripes appear along the joins between bands.

**Why.** A rectangular `\clip(x1,y1,x2,y2)` has hard, pixel-snapped edges: two bands that share an
edge tile it exactly. A *drawing* clip does not — its edges are antialiased. And a rectangle cannot
survive a bend as a rectangle, so mapping turns every rectangular clip into a drawing clip. Two bands
that used to meet now each cover the shared edge about half way, and half plus half of the same
pixel, composited, is **not** one full pixel. What is left is a lighter line down every join.

**The fix, in `MapClipBody()` (`src/typesetting_transform.cpp`):**

1. **Grow each band outward by half a unit *on screen*.** Neighbours then overlap by a whole unit's
   worth and the antialiasing sums to full coverage.
2. **Half a unit on screen is not half a unit in the source.** The pad is applied to source
   coordinates, but has to come out as half a rendered unit, so it is divided by how much the mapping
   magnifies *that part* of the picture — measured locally, by mapping unit offsets from the band's
   own corner:

   ```cpp
   Vector2D here = map(low);
   float grow_x = (map(low + Vector2D(1.f, 0.f)) - here).Len();
   float grow_y = (map(low + Vector2D(0.f, 1.f)) - here).Len();
   Vector2D pad(std::min((high.X() - low.X()) * .5f, .5f / std::max(grow_x, 1e-6f)),
                std::min((high.Y() - low.Y()) * .5f, .5f / std::max(grow_y, 1e-6f)));
   ```

3. **Clamp to half the band.** Never pad by more than half the band's own size, or a thin band would
   swallow its neighbour.
4. **Cut to the box before mapping.** A bend is only a bend inside the box it was worked out on; a
   few hundred units out — which is exactly where the ends of a gradient band sit — the same curve
   carries on into nonsense. The band is cut to `box.half * .5f + (48,48)` first
   (`CutToBox()`, Sutherland–Hodgman against the oriented box), which leaves the picture as it was
   and every coordinate where it belongs. The margin is room for what spreads past the shape itself:
   a border, a glow.
5. **Close the ring explicitly.** Push the first point again at the end, so the closing edge is
   subdivided along with the others instead of being left as one straight line across the bend. (The
   same rule applies to shapes: an implicit closing edge is why bending a quad once did nothing —
   see `CloseContours()`.)
6. **Subdivide, then map every point.** `Subdivide(segments, span)` first, because a straight run
   stays straight however it is mapped and would cut across the curve it should follow.
7. **Refuse rather than emit rubbish.** Drop the clip if fewer than three points survive, or if any
   mapped coordinate is not finite or exceeds 1e5.

Clips are only mapped when the caller asks (`map_clips`), because a clip drawn around something else
should stay where it was drawn. When they *are* mapped, every line of a stack must be mapped the same
way or the bands stop lining up — which is the other way to get stripes.

---

## 7. When to refuse

The conversion is exact or it does not happen. Refuse (with a reason the user can read) when:

* the line already mixes text with a drawing;
* it is karaoke timed (`\k \K \kf \ko`) — the syllables would be lost;
* the text is wider than the margins allow, so the renderer would break it into rows this code has
  not worked out (`LayoutWidth()`), and every piece would land somewhere the text is not;
* the outline could not be read from the font at all;
* for the decorations: the line animates (`\t`), since an animated border would have to be worked out
  afresh for every frame.

`\move` is **not** a reason to refuse. Read the position at the frame on screen and put both ends of
the run back through the same map: a run is a straight mixture of its two ends, every gesture here is
affine, and an affine map and a straight mixture commute — so moving both ends is exact on every
frame of the run, not only the one that was looked at. See `WherePlaced()` / `PlacementOverride()`.

---

## 8. Things deliberately not done yet

* Mid-line `\frx` / `\fry` (a piece turned out of the plane inside a row).
* Materialising libass's automatic line breaks as `\N` instead of refusing wide text.
* GDI vs HarfBuzz ligature differences beyond what is measured above.
* libass's ~1/64-em embolden for synthesised bold (§3).
* Matching libass's synthetic-italic angle exactly (§4).
