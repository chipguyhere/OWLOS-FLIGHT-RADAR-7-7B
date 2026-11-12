# Fork notes: Waveshare 7 / 7B and the moving layer

Engineering notes for this fork of Denis Kirin's
[OWLOS Flight Radar](https://kirindenis.github.io/OWLOS-FLIGHT-RADAR/). The README's top box is the
summary. This file holds the design, the measurements and what is still open.

## Builds

| Build | `RADAR_DISPLAY` | Board | Glass | Layout (`GFX_W`×`GFX_H`) | Scale |
| --- | --- | --- | --- | --- | --- |
| `wave7` | 15 | Waveshare ESP32-S3-Touch-LCD-7 (N8R8) | 800×480 | 534×320 | 3/2 |
| `wave7b` | 16 | Waveshare ESP32-S3-Touch-LCD-7B (N16R8) | 1024×600 | 547×320 | 15/8 |

The library detects the board at run time, but the layout is fixed at compile time, so each board
has its own build. Flashing the wrong one leaves the screen dark and the log names the right one.
Both use the CYD's 4 MB partition table (`partitions.csv`).

```sh
firmware/build.sh wave7b                              # build
firmware/build.sh wave7b /dev/cu.wchusbserial...       # build, flash, monitor
```

The 7B's text uses the 5" boards' 3/2 fonts at a 15/8 scale, so it is 4/5 of its intended size (the
Tab5 has the same compromise). A 15/8 font bake is still to come.

## Where the code is

| Path | What |
| --- | --- |
| `firmware/components/chipguy_lcd7_7b/` | chipguyhere's 7/7B board + display libraries, vendored. `shim/` supplies `Arduino.h` and `Wire.h` (TwoWire over the firmware's own `i2c_master` bus, which the GT911 shares) |
| `firmware/main/lcd_wave7_panel.cpp` | the library as a few C calls |
| `firmware/main/lcd_wave7.c` | `display.h` for these boards: the background in the frame that is not showing, flipped in when whole |
| `firmware/main/ovl.c`, `ovl.h` | the moving layer. Board-independent |
| `firmware/main/main.c` | `frame_layered()`: one pass of the ui loop per panel frame |
| `core/ui_scope.c` | `ui_layer` (BG / TOP), `main_top()`, `ticks_top()`, `ui_top_info()` |
| `core/gfx.h` | `GFX_ALPHA`: on these builds a pixel's top byte carries its coverage |
| `firmware/sdkconfig.defaults.wave7` | run-time stats (the library reads them), large plain `malloc`s to PSRAM |

Every change to shared code is behind `RADAR_OVERLAY` or `DISP_WAVE7`. The other boards compile as
before.

## The design

**Background.** Everything that stands still: the glass, rings, cross, map, panel, cards and pages. It
is drawn by the existing strip renderer with `ui_layer = UI_LAYER_BG`, into the PSRAM framebuffer that
is not being shown. It is flipped in at the next panel frame once its last strip is in, using the
library's two framebuffers. In BG mode the disc pass leaves out the sweep and the ticks, and the
planes and compass letters are skipped.

**Moving layer.** The planes (with trails, labels, glows), the compass ring's ticks and its
letters/numbers. Once per panel frame the ui task draws them with the existing primitives
(`ui_top_strip`) onto a canvas of zeros. In these builds `gfx_mix` carries coverage in the top byte
and `gfx_add` leaves it alone, so the canvas ends up holding premultiplied colour plus alpha. `ovl.c`
scans it and stores runs of 32-bit words in PSRAM. Each word is the colour spread as 565
(`0x07E0F81F`) with how much shows through (0..32) in the spare bits 5..10. Runs of eight or more
identical pixels (label cards) are stored as one word. There are three layer buffers (shown, ready,
building), and the fill takes the newest ready one at each panel frame.

**The sweep.** Not drawn by the ui task. The fill works out the trail and the bright edge for every
row from the time the panel frame started, so it turns smoothly even when the layer is late. The
trail is 32 angular steps. For each row, the x where each step's edge crosses it comes from a per-frame
tangent table, and the step's constant colour is added with a two-pixels-a-word saturating 565 add.

**Compositing.** In the vendored driver, `setOverlay()` (fork addition) gives the fill task a
callback. After the library's GDMA copy of a chunk is queued, the fill task follows the copy down the
chunk. The GDMA writes back each inbound descriptor as it completes, so `m2mLanded()` knows how many
bytes are there. Each row is composited as soon as the copy has passed it (bg + sweep + layer).
**Every row has a deadline**: the panel starts on the buffer one bounce period after its interrupt
and scans a row every period/rows, less 8 µs. A row that cannot start by its deadline is skipped and
left as the copy made it. Drawn late, it would land in the buffer after the panel had passed it, or
on top of the chunk 20 rows further down that the copy is already putting there. That was the
"bleeding downward" seen before the deadlines existed.

**Prepare, then apply.** Each row's work is in two halves. The part that needs no pixels (the
sweep's spans and edge, and fetching the row's layer words out of PSRAM into internal RAM) runs
while the copy is still on its way. The part that needs the pixels runs once the row has landed.

**The next copy starts early.** Between rows the fill looks at its queue. If the next chunk's
interrupt has already come, its copy is started at once instead of after this chunk's overlay. This
took the rows that were late waiting for the copy from ~11,000 to ~100 per 10 s.

**Cores.** Core 1 is the panel's: the bounce interrupt and the library's fill task (started from a
short task on core 1 before the WiFi, as the library's `lv_setup` does) with the compositing, and
under it, at the lowest priority, the **background task**. That task draws from its own copy of the
ui state (`ui_t`, 14 KB, in PSRAM), taken when it is ready for another frame. Its 4-row strip
buffer is in internal RAM, its stack in PSRAM. Core 0 runs the ui loop (the moving layer only)
beside the WiFi. The layer being drawn is a field of the canvas, not a global, because both cores
draw at once.

## Changes to the vendored library

All are marked "fork addition". Candidates to upstream:

- `setOverlay(fn, ctx)`, `_overlayRows()`, and the `g_ovl*` counters.
- `M2MXfer::nd` and `m2mLanded()`: the progress of the newest m2m chain, from the GDMA's write-back.
- Bounce rows built at 10, not 20 (`LCD7_7B_BOUNCE_ROWS`, a compile definition in the component),
  for internal RAM.

## Measured, -7B, 2026-11-11

Demo sky (no WiFi set), 12 planes, radar look, latest build:

| | |
| --- | --- |
| Panel | 32.7 Hz, 0 fills dropped, 0 desyncs |
| Worst chunk, interrupt → overlay done | ~0.95 ms (deadline-bounded) |
| Rows without the sweep (skipped as late) | 0 per 10 s in the latest runs (live data, few planes) |
| Rows with the sweep but not the layer | 0 per 10 s in the latest runs; the sweep goes first, the layer only if its words fit before the row's deadline |
| Of those, waiting on the copy | ~100 per 10 s |
| Fill: sweep / layer composite | ~3.6–4.1 ms (trail on alternate rows, `OVL_SWEEP_EVERY 2`) / ~3.5–3.8 ms per panel frame |
| Layer words | ~14.5k a frame, with the DEMO card's box a hole (~28k without) |
| ui loop (core 0) | layer made in ~62 ms, so it changes at ~15 fps |
| Background (core 1) | a whole frame every ~1.9 s |
| Internal RAM left | ~8 KB, **~3 KB at the least**: too tight |

How it got there, for the record: a CPU copy of every chunk took the whole core (thousands of late
chunks). Waiting for each chunk's whole DMA before compositing made copy + composite longer than a
period. libm calls in the fill (running from PSRAM under XIP, behind the busy DMA) cost a third of
the sweep. The ui on core 1 beside a fill that spins on the DMA starved. Before the per-row
deadlines, late rows bled into the next chunk sharing the buffer. Before the early start, the next
chunk's copy waited for this chunk's overlay.

## Open

1. **The layer is not frame-locked yet** (15 of 32.7 Hz). Making it costs ~62 ms on core 0. Likely
   costs: every band re-walks every plane and all 72 ticks; the scan of the bounded box; code
   running from PSRAM. Next: per-frame lists of what touches which band (ticks especially), and
   profiling `planes()` per band.
2. **The background redraws every ~1.9 s** on core 1, below the fill. Settings pages have no moving
   layer, so they show at that rate too.
3. **~3% of overlay rows are skipped**, mostly in the middle of the screen, where the disc is
   widest and the labels gather. They are CPU-bound. The trail is now on alternate rows
   (`OVL_SWEEP_EVERY` in ovl.c, 1 for every row): its cost fell from ~6 to ~3.8 ms. The layer
   composite (~6.5 ms, ~28k words) is now the larger half. Next: fewer layer words (glows are the
   bulk after labels), and the S3's 128-bit PIE for the flat runs and the sweep spans (as in the
   library's `RasterBouncingBalls`).
9. **XIP and the PSRAM bus.** With XIP, code and rodata misses come from PSRAM, but the fill's hot
   path is in IRAM. Flash and octal PSRAM share the S3's memory bus and cache, so moving code back to
   flash would not relieve it. An SD card for the tiles would only matter if flash writes stalled
   the panel, which XIP already prevents. The late rows measure as CPU-bound, not bus-bound.
10. **The screen goes dim on its own sometimes and does not come back** (the 7B, seen twice). Re-sending
    the backlight every 10 s does not bring it back. The 10 s log now says whether the expander took
    the write, the expander's lines (backlight, panel reset, panel supply), and the battery-input ADC:
    first readings all acknowledged, lines as they should be, battery input moving between 4.16 and
    4.49 V with no battery fitted. If the lines stay right while it dims, suspect the power path (the
    7B's charger, or the USB port's current under a 7" backlight).
12. **Popups are holes in the moving layer.** `ui_top_info()` lists the boxes the app draws over the
    scope (the DEMO card, a toast, the update note), and neither the sweep nor the layer is put in
    them. They are drawn after the planes and the sweep in the original too. Their 5-px chamfered
    corners are not cut out.
13. **The sweep comes first.** Each row has a deadline from the library (`g_ovlRowDeadline`): the
    sweep is always applied, the layer only if its words fit, about 30 cycles a word.
11. **Chamfered corners at 15/8.** The scaled `gfx_hline`/`gfx_vline` weight each glass row and
    each end pixel by how much the layout pixel covers it. At scales other than 3/2, `chamfer_box`
    draws all six sides with `gfx_line` through the pixels' middles, so the cut corners meet the sides
    (checked in a magnified render of `gfx.c` built on the Mac). The 3/2 boards draw boxes as before.
14. **Prepare-ahead.** While a row's copy has not landed, the fill prepares up to two rows ahead
    (`LCD7_7B_OVL_PREP_AHEAD`, a three-row ring in ovl.c; layer words are pulled into the cache, not
    copied). Per panel frame it was spending ~4.7 ms preparing, ~4 ms applying and ~10 ms spinning on
    the copy. Late rows now come in bursts in ~70–130 chunks of ~20,000 per 10 s, most likely rows
    of dense label text (~1,000 words). Next: the S3's 128-bit PIE for the per-word composite.
15. **RAM.** The moving layer's band is 6 rows (24 KB). Big state no interrupt touches is in PSRAM
    on these builds (`CORE_BIG` in core/gfx.h, `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`): `s_ui`,
    the sky and its reader, the map and its plan, the ring tables, the background's gradient and
    vignette, the grid. Internal RAM: ~24.8 KB free in steady state, 6.9 KB at the least (was 611
    bytes). Less was gained than those symbols' ~65 KB; not looked into yet.
16. **PIE composite.** Layer runs of 16 pixels or more are stored as two 16-bit arrays (colour,
    see-through amount), lined up with the panel row on 16-byte boundaries. A PIE kernel built only
    from instructions `RasterBouncingBalls` already runs on the S3 composites them 8 pixels at a time,
    bit-exact with the C version. A self-test at start compares the two on 4,096 random pixels and
    falls back to C if they differ (it passes on the 7B). The layer's composite fell from ~4 to
    ~0.7 ms a frame.
17. **Nothing to overlay, nothing to wait for.** The overlay answers `ovl_want()` per chunk; a chunk
    with no layer words and no sweep in its rows is copied as the library normally does, without
    following the copy. Off the radar page (settings, keyboard) the fill no longer holds core 1, and
    the background task that draws those pages gets it.
18. **The sweep's bright edge as spans.** Near 3 and 9 o'clock the edge runs along a row (hundreds
    of pixels). It was worked out a pixel at a time, at most 96 a row; now its coverage is stepped by
    adding, and pixels of one brightness become one span (applied two pixels a word).
20. **The busiest rows, pre-rendered.** When a layer is made (core 0, at most every 100 ms), each row's
    cost to the fill is estimated (~300 cycles a run + ~60 a stored word, measured). Rows over
    `OVL_BUSY_CYCLES` (~17 µs), whose covered span is no more than twice their stored words, get that
    span made whole: the shown background with the layer on it, in a PSRAM row cache (up to
    `OVL_BUSY_ROWS` 192 a layer). The fill copies the span into internal RAM during prepare and onto
    the row once it lands, then adds the sweep, which on these rows lies over the planes. Whole rows
    were tried first and were far worse (a 2 KB PSRAM read each; ~55,000 late rows per 10 s). With
    48 planes: ~150–170 rows a layer, ~190–250 rows without the sweep and ~120–220 without the layer
    per 10 s, from 1,400–3,900 and 4,500–9,800.
21. **Crashes, found with the core dump on flash.** The watchdog, each time core 0's idle task: the
    layer built back to back, then the menu page drawn back to back, beside TLS handshakes in the fetch
    task. Fixed by pacing (the layer at most every 100 ms counted from the end of a build; a page
    drawn on core 0 leaves the core free for half its draw time, at least 50 ms). These builds no
    longer have the watchdog watch core 0's idle task; the ui task stays watched. `chamfer_box` fills
    its six-sided shape row by row: the smooth polygon fill made a keyboard page take seconds.
22. **The heaviest chunks of the next frame, composed beforehand on core 0.** A planner task on core 0
    (priority 6: above the ui task, below every network task), woken at each panel frame's start, plans
    the next frame: its layer, and where its beam will be (both latched by the fill at that frame's
    start, so composed and live chunks agree). It estimates every chunk's cost to the fill from the
    internal RAM only: the sweep from its spans, the layer from a per-row cost table made with the
    layer. It composes the 8 heaviest over `PRE_CYCLES` (`PRE_PICK`), in scan order, into 64-byte-aligned
    PSRAM chunk buffers: the background's rows, the sweep, and the layer too where that chunk's layer
    is heavy (many planes; `PRE_LAYER_CYCLES`), else the fill adds the layer. The library's new `src`
    hook copies such a chunk from its buffer instead of the framebuffer; a chunk composed whole is a
    plain copy, the core yielded. Frozen 48-plane test sky, per 10 s: rows without the sweep 40–82
    without this, 1–10 with it. Lessons on the way: core 0 reading every layer word every frame to
    estimate costs, composing ~20 chunks a frame, and running the planner from the ui loop (it missed
    two frames in three) each made the panel later, by PSRAM traffic or by the plan not being there.
23. **A test sky.** `OWLOS SKYSAVE` / `OWLOS SKYPLAY 1` / `OWLOS SKYPLAY 0` on the serial console keep
    a live sky in NVS and show it, still, instead of the network: builds can be compared on the same
    scene (repeatable to about ±15 rows per 10 s).
19. **Networking.** TLS uses software AES on these builds: the hardware AES took a DMA buffer from
    internal RAM for data in PSRAM, which failed ("esp-aes: Failed to allocate memory"). Wi-Fi modem
    sleep is off on these boards. On every board, Wi-Fi now scans all channels and joins the
    strongest access point of the network (`WIFI_CONNECT_AP_BY_SIGNAL`): ESP-IDF's default joined the
    first it heard, here one at -85 dBm with another at -47 dBm a few metres away. This last one is a
    general fix the original project may want.
4. **The radar's ground echo is gone.** The original lit the map's land and roads brighter just
   behind the beam, per map class. The fill has no map classes, so the trail is the same colour over
   all of it.
5. **The trail has 32 steps and no dither**, so its fade can band slightly.
6. **The -7 runs** (2026-11-11): 22-23 fps on the demo sky, no late rows, 2.4 MB of PSRAM heap free. A build
   folder made before `sdkconfig.defaults.wave7` existed kept the old settings and hung on a dark screen;
   lcd_wave7.c now refuses to build with them.
7. **RAM is tight**: 64 KB of strip buffer left no room for the ui task's stack. The strip/band
   buffer is shared (12 rows, 48 KB).
8. XIP from PSRAM is on (upstream's `s3rgb` defaults). Map tiles and settings are written to flash.
   Check the late/dropped counters while tiles are being cached.

24. **Beam step on by default; the compass ring follows a finger.** `beam_step` (a plane moves only
    when the beam goes over it) is now on in `scope_opt_defaults`; a board with options already saved
    keeps its own setting. While a finger turns the compass on the rim, the moving layer is the ring
    alone (`ring_only` in `ui_top_t`) and is made again every pass instead of every ~270 ms, so the ring
    turns at the panel's pace; the planes leave the layer meanwhile and come back, turned, once the
    finger lets go. The background (map, cross) catches up at its own pace (~2 s a frame on the -7B).
