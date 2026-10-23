# HD Features: Stamps

The stamps part of **HD Features** (`local.ricky.hdfeatures`, `mods/hdfeatures`). It began as its own mod,
HD Stamps (`local.ricky.hdstamps`); see the repository `README.md` for the install and `BUILDING.md` for the build
(each option is explained by its tooltip in the mod's panel). The sources are `mods/hdfeatures/src/stamps/`
(namespace `hdstamps`).

Adds Twilight Princess HD's 50 stamps to the GameCube-based PC port. The model, icons, stamp art and HD's in-game
text are not in the mod. At the first start, the mod makes them on your PC from your own TP HD, into Dusklight's
data folder (`mod_data\local.ricky.hdfeatures\files-<id>\`). Those game files stay on your PC: never share them.
The `.dusk` itself has no game files in it. Paths like `res/stamps/...` and `overlay/res/Object/Obj_Stamp.arc`
below are inside that folder. The Mods panel's **Stamps** switch turns all of it off and on again without a reload.

## What it does

- **46 stamp chests.** 39 GC chests that HD restocked give their stamp instead of rupees (only
  if the chest still holds its GC item and is in the expected room). The 7 HD-only chests are
  added from HD's own records, which the mod copies from your HD copy into
  `res/stamps/stamps/hd_chests.bin`: Cave of Ordeals floors 10/20/30/40/50, plus two grottos.
  Once you own a stamp, its chest gives GC's rupees again.
- **Other stamps.**
  - Rupee: from Gengle in Jovani's house, once you have all 60 Poe souls. As in HD, the cat adds
    HD's own line for it (as a second page of its silver-rupee talk) and the stamp's
    get-item scene follows when the talk ends.
  - Agitha: after 12 golden bugs. Her talk right after the 12th bug's reward opens with HD's own
    three lines, then she hands the stamp over herself, as in HD.
  - Malo Mart: your first purchase at the Castle Town branch. The clerk adds HD's line about the
    stamp to the "thank you" and its get-item scene follows. With HD's text, bombs and arrows
    count too; without it, only purchases that hand over an item do.
  - Gengle's and the clerk's lines are said once, as in HD. If you buy again before the stamp's
    scene has played, that purchase gets GC's "thank you" only.
  - Twili Midna: added by itself once you own the other 49, with HD's 49-stamp message.
- **Hyrule Castle seeds.** As in HD, the slingshot seeds that GC kept in Happy Zelda's chest are in
  chest 29 next to it, which held a green rupee on GC.
- **Get-item scene.** Each stamp has its model (`Obj_Stamp`), item-get icon, a short jingle and
  HD's own get-item text. Like HD, that text has no stamp count; the count is in the stamp book.
- **HD's stamp book** (see "Stamp book" below): the stamp slot on the Collection screen, HD's
  own book layout drawn natively, and the automatic openings after the 1st and 49th stamp.
- **File select.** The Quest Log details show HD's stamp plate with the file's `N/50`, and + (Start)
  opens that file's stamp book read-only, as HD's "+" on the plate does.
- **HD's own text.** Item-get text, stamp names, the book's title and count, and what Agitha,
  Gengle and the Malo Mart clerk say are HD's text, read from `res/stamps/hdtext/messages.bin`. The
  mod makes that file from your TP HD with the others. Without it, the mod uses
  its own wording, the old "N of 50" get-item count, and no NPC lines. The Troubleshooting section of
  the HD Features panel shows which text is in use and whether Agitha's lines are patched
  in.
- **Recovery.** Each time a save loads, the mod grants any stamp whose chest is already open in
  that save, plus Malo Mart (HD switch 0x7F of table 9) and Agitha (12 bugs).
- **Save safety.** Ownership lives only in SaveService blob `"stamps"`
  (`<card>/…gczelda2.mods/local.ricky.hdfeatures.json`, next to the cave's blob `"cave"`). Stamp ids never get "first get" bits in the
  `.gci`; the only new `.gci` bits are the 7 HD-only chests' opened flags, which are unused in GC.
  The `.gci` still loads with the mod off.
- **Like HD**, a stamp you got after your last save is taken back if you reload that file without
  saving in the same session (its chest is closed again, so it can be re-earned).
- **Vanilla game mode only** (save name `gczelda2`). The mod does nothing in the randomizer, the
  speedrun mode or other mods' modes, and gives the 50 item ids back to them.

## Stamp book

As in TP HD, there is no separate stamp menu:

- **Collection screen (Start).** A 5th slot right of the Letters (GC grid cell (4,4); HD's
  (4,3)). Its empty frame is always there; the stamp icon and its 2-digit count appear with the
  first stamp. Name "Stamp Collection" (2800), description 2801 with your `N/50`, A = "Open".
  The item descriptions move right by one slot and shrink slightly, as HD's did.
  Moves from the slot follow HD's Collection screen: Up goes to Hidden Skills (the nearest cell of
  the row above), Down to Save, Left to the Letters; the only way in is Right from the Letters.
  GC's "Up from Save returns to the cell you came from" rule would re-enter the slot after Down
  from it; HD has no such rule, so Up from Save then goes to the cell above Save (as when GC has
  no previous cell).
- **A on the slot:** the sub-window transition GC uses for the Letters (SUB_IN sound, fade), then
  HD's book: HD's four layouts, loaded unchanged from the converted `stampResR` archive.
  - 5 pages of 2x5 stamps; missing ones show "?" and no name; the selected one is larger, in black
    ink, in the gold frame, with the corner cursor. The stamp's HD name is shown above.
  - Stick or D-pad moves (no wrap, never changes page), L/R change the page (wraps 5 -> 1); as in
    HD only one of these is taken per frame (Left, Right, Up, Down, L, R in that order). A does
    nothing on a stamp, B closes (SUB_OUT, fade, back on the stamp slot).
  - **Post (Miiverse).** As in HD the plate is there and Down from the bottom row selects it
    ("Post" next to a white A); A on it does nothing and plays nothing, which is what HD does when
    the Miiverse call fails. "Greyed out" and "Hidden" are options.
- **Automatic openings (HD).** When the item-get window of your 1st stamp closes, the book opens
  by itself once the event is over (view-only: any button closes it; a direction you were already
  holding while walking only counts once you let go of it). After the 49th stamp it opens on page 5
  for Twili Midna's reward: once the fade-in has finished, the item-get fanfare, the stamp fading
  in, then HD's message (2806). Both return straight to the game. As in HD the request uses the
  gates of Start: never during an event or cutscene, with the map up, where the Collection screen
  is not allowed, or with a stage change pending. If the reward sequence cannot run (missing
  assets or hooks, or no 2D heap at that moment), HD's 2806 text is shown as a notice instead,
  whatever the toasts option says.
- **File select (HD's "+").** In a file's Start/Copy/Delete menu, + (Start) on a file with at least
  one stamp plays HD's 0xAA, fades out and opens that file's book read-only (its own stamps, from
  its blob); B fades back to the menu. The "+" glyph is only shown while this works. Option
  "+ (Start) on the file select opens the stamp book".
- **Fallback.** Without `res/stamps/stampbook/` (or if a hook is missing) the stamp list takes over: it
  is two tabs of the **HD Features** window (menu bar), and the panel's "Open the stamp list" button
  opens it; Troubleshooting says why. The list (with its per-stamp checklist and hints, not HD) is also
  available as the "Stamp list in the menu bar (debug)" option.

Options (HD Features panel, "Stamps: options"; config keys `mod.local_ricky_hdfeatures.<name>`):

| Option | Key | Default |
|---|---|---|
| Stamps (the switch) | `stamps-enabled` | on |
| Open the stamp book by itself | `stamps-book_auto_open` | on |
| Post button | `stamps-book_post_button` | 0 = shown (HD); 1 greyed out, 2 hidden |
| Stamp count on the file select | `stamps-file_select_counter` | on |
| Stamp book on the file select | `stamps-file_select_viewer` | on |
| New stamp notices | `stamps-toasts` | off (HD has none) |
| Collection-screen stamp slot (next time the Collection screen opens) | `stamps-collection_slot` | on |
| Stamp list in the menu bar (debug) | `stamps-debug_stamp_list` | off |
| (hidden) last menu scaling seen | `stamps-last_menu_scaling` | 1 = Wii |

The hidden `stamps-last_menu_scaling` remembers the menu scaling mode last seen on the Collection screen
or the file select, so an automatic opening early in a session uses the right one.

Memory: the book archive (about 11.9 MB) stays in process memory and is only mounted while the
book is open (its archive object on the archive heap). The book itself is built on the pause
menu's 2D heap (it refuses to open below 256 KB free, or the measured use + 64 KB once it has
opened; the log line "2D heap X -> Y bytes free" shows the use), or on the file-select viewer's own
512 KB child heap of the game heap.

## How the files are made

At the first start, the mod's converter (`mods/hdfeatures/convert`) makes the stamps' game files from your TP HD:
`overlay/res/Object/Obj_Stamp.arc`, `res/stamps/stamps/*.png` and `hd_chests.bin`, the 50 item-get icons
`res/stamps/stamp_icons/stampNN.bti` (48x48 CMPR), `res/stamps/hdtext/` (HD's text) and `res/stamps/stampbook/`
(the book archive, about 11.9 MB, plus the slot and file-select textures). Its spec is the Python pipeline
`tools/build_assets.py`: each file the converter makes is byte for byte the pipeline's (`BUILDING.md` shows how to
compare the two).

The stamp table `src/stamps/stamp_table.inc` was generated by `tools/gen_stamp_table.py` from the developer's own
research tables, which are not in this repository.

## Known limitations

- **Since the merge into HD Features (2.0.0), the stamps were checked in short automated game runs.** They were
  last play-tested as HD Stamps 1.0.0.
- **Stamp book details that differ from HD:**
  - The slot uses GC's 42x42 cell metrics (HD's cells are 36x48) and GC's grid, so Up from it
    reaches Hidden Skills where HD reaches the Scent.
  - The cursor brackets sit on the slot's centre (Dusklight re-centres them every frame); HD
    offsets them by (-1.75, -1.5).
  - No mouse inside the book (HD's GamePad taps); the mouse opens it from the Collection slot.
  - At 16:9 in Wii/Dusklight scaling, the stamp name, the page text and the L/R glyphs keep their
    4:3 shape like HD's `n_43` panes (not proven against HD footage); the reward popup's dim
    always covers the whole screen.
  - The file-select viewer's button (+ = Start) and its close sound (SUB_OUT) are inferred.
  - The book follows the menu scaling mode last seen (Collection screen or file select, kept across
    sessions); if Dusklight's setting is changed and neither screen has been shown since, the old
    mode is used once.
- **Cosmetic differences from HD:**
  - The 9 chests HD enlarged (small to big model) stay small.
  - The Snowpeak "Q" chest and two other chests HD moved slightly stay at their GC spots.
  - The HD-only chests have no minimap marker.
- **Malo Mart and Gengle triggers are this mod's own.** GC has no shop slot or flow for these
  stamps. HD sells the stamp on a shelf for 50 rupees; here the first purchase earns it, so HD's
  shelf description is not used. Gengle's line is added to the cat's silver-rupee talk instead of
  a new talk, because the cat only drops its rupee when the talk ends on GC's own line.
- **English text only.** It is shown for every language setting, as the own wording was. A US
  disc only has English anyway.
- **Agitha's lines need FlowService.** Her flow is patched only while a vanilla save is selected.
  The mod reads her flow's start from every `zel_04` message folder on the disc (a US disc only
  has `Msgus`; one of `Msgus` and `Msguk` must exist) and patches it only if they all agree. If
  that fails, she gives the stamp through the regular get-item scene without the lines, as
  before, and the mods panel says "without Agitha's lines".
- **Get-item scene fallback.** A stamp whose scene never starts is granted after about 1.5 minutes
  of quiet play, without the scene. If the host plays the scene later anyway, it shows a stamp you
  already own; nothing is counted twice.
- **Reload detection.** Stamps are only taken back on a reload within the same session. If the
  game is closed without saving, and another save had already flushed the mod data, the stamp is
  kept and its chest pays rupees.
- **Missing or bad model.** Without `Obj_Stamp.arc`, or when it fails validation, stamps show GC's
  rupee placeholder model.
- **Don't toggle the mod mid-scene.** Disabling or reloading the mod during a stamp's get-item
  scene can release the wrong item archive. Disabling it while Agitha is talking ends her talk. (The
  **Stamps** switch itself waits for a moment without a menu, event, message or open stamp book; turning
  it off also waits until no stamp is being handed over, and until you leave the Cave of Ordeals
  (`D_SB01`) or a grotto of the two grotto stages (`D_SB06`, `D_SB07`) that have HD's extra chests, so a
  chest already in the room never gives an empty item.)
- **Saving while the Stamps switch is off** stores no stamp data: saving into a *different* file than the
  one loaded then leaves that file's old stamps (or none) next to the new game. Turn the switch back on
  before saving into another file.
