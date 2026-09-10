# UI editor

Lays out `core/ui` widgets on a defined surface and saves them as a `UiDocument` that a game loads
and draws through the same code path. Run from the repository root:

    make run

## The surface

A layout is not a free-floating pile of rectangles: it targets a surface of a stated size and style,
and element coordinates are relative to that surface's content area.

- **Screen** — no chrome, drawn straight onto the game's own image. Shown over a checker so it is
  clear the background belongs to the game.
- **Panel** — a raised face with a frame.
- **Window** — a frame with a title bar carrying the document title.

The workspace draws that surface at true size with its real chrome, so what is on screen is what the
game shows. `UiDocumentDraw` is the same call the editor and the game make. Set the size, style and
title in Properties.

## Placing and arranging

Choose a type in Elements (the armed type stays pressed; Escape cancels), then click the surface.
Select by clicking, drag to move, and drag any of the eight handles to resize. Clicks on the
toolbar, panels or status bar never reach the surface.

With **Snap** on, a dragged element locks onto:

- the grid, whose pitch is the theme's `containerGap`,
- another element's left, right, top, bottom or centre line — a blue guide shows what matched,
- the conventional `containerGap` spacing after a neighbour, so stacked rows keep their rhythm,
- the edges of whatever contains it, and the same conventional inset from them,
- whole `itemHeight` rows when resizing vertically.

## Containers

Indent and Window elements are containers, and an element whose centre is inside one is arranged
against **that** container rather than the surface. Inside an indent there is no padding and no gap:
contents sit flush against the bezel and flush against each other, which is what `UiGroup` produces
in code — a face gap outside, one indent, contents flush within it. A Window element reserves its
frame and title bar the same way. So dragging a button into a well locks it to the well's inner
edge, `Fill` spans the well exactly, and align right means the well's right edge, not the surface's.
The innermost container wins when they nest.

The toolbar carries the arrangement commands, which all work on the selection and all obey its
container: `L C R` and `T M B` align, `Fill` spans the content width, `Row` sets one conventional
row height, and `Stack` puts the element under the previous one sharing its x and width. `Dup`,
`Raise`, `Lower`, `Delete` and `Undo` follow. Right-clicking the surface opens the same commands as a
nested menu.

Properties is real text entry throughout: click into a box to place the caret, drag to select,
`Ctrl+A` select all, `Ctrl+C`/`Ctrl+X`/`Ctrl+V` through the system clipboard, arrows and `Home`/`End`
to move, `Backspace`/`Delete` either side of the caret, and the view scrolls to follow the caret.
raylib supplies the pieces — `GetCharPressed` and `GetClipboardText`/`SetClipboardText` — so there is
no extra dependency. The numeric boxes are the same field: type a value, or use the steppers beside
them, and the box tracks the value while you are not editing it.

Pasting preserves UTF-8 characters and stops before a character that cannot fit in the field's
buffer. The default theme starts with ASCII and Latin-1, then loads additional bitmap glyphs from
its external font as text needs them. Characters absent from the font still display as `?`.

## Resizing

Buttons and labels have **Text X** (`L`, `C`, `R`: left, centre, right) and **Text Y**
(`T`, `M`, `B`: top, middle, bottom) selectors in Properties. These align the text within
the element, not the element within its parent. Buttons keep their bevel and text spacing.
Alignment appears in preview and survives duplication, undo, and saving.

The authored surface size is the baseline every element is placed against. Drag the grip at the
surface's bottom-right corner to *test* another size: the window itself grows or shrinks from that
corner, its top-left staying put, and the layout re-resolves live. The caption reads
`TESTING 348x180 (was 320x200)`, and editing is suspended because anchors only mean something
relative to the baseline. Escape returns to the authored size. To change the baseline itself, use
the surface W and H in Properties.

**Nothing is scaled.** A resize changes the client area, exactly as `WM_SIZE` does on a desktop:
text stays 16 px, a row stays `itemHeight` tall, bevels stay 1 px, and the extra space is
redistributed by the anchors. The workspace blits the canvas at whole pixels only, so a squashed
glyph is not something this editor can draw — there is a check that the canvas and the tested
surface always agree in size.

Each element carries four **anchors** — `L R T B` in Properties — saying which edges of *its
container* it keeps its distance from:

- one edge pinned: the element keeps its size and rides that edge,
- both edges of an axis: the element stretches by the whole change,
- neither: it floats with the middle of the region.

New elements default to **left, right and top**: they follow the width of whatever holds them and
keep their own height, which is how a block element behaves in a page and how a width-sizable view
behaves in GNUstep. So a fresh layout already responds to a resize without anchoring anything by
hand. Untick `R` for a control that must keep its width, tick `B` as well for one that should ride
the bottom edge, and tick both of an axis to make it fill.

Anchors resolve against the innermost container, so an indent that stretches carries its contents
with it.

Two things pinned to the same pair of edges would each want the whole change, and would run into
each other. Siblings that stretch on the same axis divide the change between them instead, in
proportion to the size they were drawn at, so every gap between them stays exactly as it was.

Who divides with whom is read off the layout: your queue along an axis is what follows you on it
*and* shares your band across it. Two panels side by side divide the width and each take the whole
height; stacked, they divide the height and each take the whole width; and in a four by four grid
each cell divides width with its row and height with its column, so rows stay level, columns stay in
line and nothing overlaps. New containers are pinned to all four edges, so they fill what they are
given in both directions straight away — untick `T` for something that should ride the bottom
instead.

An indent, though, is a region its contents **share**, so by default they divide it rather than obey
their own anchors: a bar of four buttons shares the width equally until a button reaches its own
minimum; that button holds its width while the others keep shrinking. Buttons stay flush to each
other and to the bezel. That is the convention — no padding, no gaps — held under
resize, and it is why a stretched button bar grows its buttons instead of leaving a gap at one end.
The axis is read off the contents (side by side divides left to right, stacked divides top to
bottom); the `Shares` button in Properties pins it to row or column, or to `nothing` when you want
the contents left exactly where you put them. An odd remainder is spread a pixel at a time, so the
unconstrained cells differ by at most one and never leave a hole. A window element leaves its contents alone.

**Minimum sizes** (`w` and `h` in Properties) stop a layout collapsing. The editor works the first
one out from the label — the text width plus room for padding and bevels — and writes it into the
box, where you can read it and type over it. It follows the label while you rename, and stops
following the moment you set a number yourself. Core does not guess: it only reads what is in the
layout.

**Maximums** (`w` and `h` under *Most*) are `0` — no limit — until you set one. A capped element
stops growing exactly at its limit and hands what it did not take to the others in its queue, so the
gaps either side stay as drawn and the row still reaches its margin.
Shared rows and columns also enforce maximums on both axes: uncapped siblings take the spare
space. If every child is capped, unused space stays at the right or bottom of the indent.
An explicit maximum takes precedence over a larger minimum, so a maximum of 24 stays 24 even
if the label would need more room.

`UiDocumentMinimumSize` folds those minimums up through the containers — a shared region sums its
children's individual minimums along its flow and takes the largest across it — and adds the authored size of
anything that cannot stretch, because that never gets any smaller. The grip refuses to drag below
the result, so a window can ask the document how small it is allowed to get. A bar of buttons can
therefore shrink to what its labels need rather than being stuck at the widths they were drawn at.

Resolving never edits the authored rectangles: `UiDocumentResolve` fills a separate array, and
`UiDocumentDrawSized` draws through it. A game that wants a fixed layout keeps calling
`UiDocumentDraw` and centres it, as `game2d_dodge` does.

## Zoom and panning

The zoom button cycles 1x, 2x, 4x, 8x; the mouse wheel over the workspace and `+`/`-` step through
every whole factor between. The surface is drawn once at true size into a texture and blitted at an
integer factor with point filtering, so a doubled pixel is exactly four pixels and single-pixel
bevels survive. There is no fractional zoom: half a bevel is not a thing this UI can draw. Selection
handles stay screen-sized so they remain grabbable, and the pointer is mapped back through the zoom,
so clicking and dragging land on the pixel you are pointing at.

Drag with the **middle button** to pan. The surface is centred until you move it, zooming keeps the
middle of the workspace where it was, and panning stops before the surface can leave the workspace
entirely. A surface larger than the workspace is drawn under the panels rather than over them, so
pan is how you reach its far side.

## Keys

`P` preview, `G` grid, `S` snap, `+`/`-` zoom, middle-drag pans, the corner grip tests a size, `Tab`
cycles the selection, arrows nudge (Shift by the grid pitch, Ctrl resizes), `Delete` removes,
`PageUp`/`PageDown` reorder, `Ctrl+D` duplicates, `Ctrl+Z` undoes,
`Ctrl+S`/`Ctrl+O` save and load `ui_tool/layout.ui`, `Ctrl+Q` quits. Escape backs out one
step: it closes a menu, cancels placement, leaves preview, then clears the selection. While a text
field has focus it owns the keyboard and none of these fire.

**Preview** makes the widgets live: buttons press, sliders drag, checkboxes toggle. The caption above
the surface and the right of the status bar both say which mode you are in.

## File format

Text, versioned, one element per line, tab before the label:

```
core_ui_document 5
surface <width> <height> <style>	<title>
<type> <x> <y> <w> <h> <value> <checked> <anchors> <minW> <minH> <flow>	<label>
```

Version 1 files (no surface line) and version 2 files (no anchors) still load, taking the same
default anchors as a new element. Versions 3 and 4 also load. Layouts without text alignment
keep centred buttons and left-aligned labels, vertically centred. Version 5 adds maximum
width/height and horizontal/vertical text alignment after the flow field. Alignment values
are 0 (start), 1 (centre), 2 (end).
The resulting `.ui` document can be loaded by any project using the engine's UI document API.

## Checks

```sh
./build/ui_tool --smoke
```

Runs the tool with scripted pointer, text and keyboard input for 59 frames and asserts the
behaviour rather than the pixels: placement clamps into the surface, chrome clicks never leak onto
the surface or drop the selection, clicking the label field keeps the element selected and typing
renames it, dragging locks to a neighbour's edge and to the conventional gap, dragging into an
indent locks flush inside its bezel, arranging inside an indent uses the indent, resizing settles on
whole rows, undo restores geometry, the middle button pans, hit testing maps back through the zoom
and the pan, anchors move and stretch and float the right elements, a row flow divides its indent
flush with the remainder spread, minimum sizes account for the contents, dragging the grip tests a
larger surface without touching the authored rectangles, and documents round-trip. Captures of the
edit, drag, preview, zoomed and resized states go to `build/ui_tool*.png`. It is part of `make smoke`.
