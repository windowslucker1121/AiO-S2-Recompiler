# Menu pointer: engine and adapters

The mouse as a pointer in a game's own menus: hover selects, left click confirms. Shared code in `src/gfx/play/`:

| File | What it is |
|---|---|
| `menu_mouse.h`, `menu_mouse_math.cpp` | The pointer itself (events, the window to UI plane mapping). Unchanged. |
| `menu_pointer.h`, `menu_pointer.cpp` | The engine. It knows no game and no UI library. |
| `menu_ktgl.h`, `menu_ktgl.cpp` | Adapter for KTGL screen layouts (Koei Tecmo). Used by Pokopia. |
| `menu_ui2d.h`, `menu_ui2d.cpp` | Adapter for Nintendo's nn::ui2d layouts (BFLYT) and the button controls built on them. Written for Donkey Kong Bananza. Not wired into any game yet. |

The game module supplies the addresses and wires the adapter into play mode's `GamePlay` table (`menu_frame`, `menu_over_value`, `menu_active`, `menu_in_game`). Pokopia's wiring is `games/pkp/pkp_menu.cpp`: a `ktgl::Config` holding the offsets of its 2.0.1 main, plus `play_follow_live` as the gate policy.

## Engine

Once a frame, `Engine::frame(menu_up, geometry, adapter)` does the following:

1. It takes the pointer snapshot. If the pointer is not active, or the adapter is not `ready`, it calls `idle`, drops every element and clears the navigation.
2. `collect` registers the frame's elements (`Engine::add`) and the game's selection (`set_selection(focused index, marker on screen)`). It returns whether a menu is on screen; the gate reuses that answer. Elements live for one frame only.
3. Hit test. The hovered element is the smallest `kHover` element under the pointer; on a tie the first one registered wins. The slider is the first `kSlider` element under the pointer.
4. Hover then focus. When the pointer moves onto another element while a marker is up, that element becomes the target:
   - With an adapter `focus` call, the engine calls it once and waits 8 frames for the selection.
   - Without one, it presses the d-pad toward the target, one press then 8 frames of waiting. If a press moved nothing, the next one tries the other axis. It gives up after 4 failed presses or 40 presses in total.
5. Click then confirm. Rules in order:
   - An element with its own `click` button (a key guide) under the pointer sends that button.
   - With no marker on screen, a click is A.
   - A click on the selected element confirms it: the adapter's `confirm`, or A.
   - A click on another element selects it first. It is confirmed once the selection has stayed on it for 2 frames; after 90 frames without that, the click is dropped.
6. Wheel. `over_value()` is true while the pointer is over a slider. `play.cpp` then steps the value with the wheel instead of moving the selection.

`Gate::poll` covers menus the game opens during play. The game keeps calling its input function there, so play mode cannot tell those frames from play by itself:

- While closed, the gate polls the adapter's scan every 6 frames.
- While up, it reuses the menu frame's own scan of that frame.
- It opens after 2 positive polls in a row and closes after 10 negative frames.

## KTGL adapter (Pokopia)

This is the old `games/pkp/pkp_menu.cpp` logic, moved over as it was:

- the pool walk, the draw hooks and the per-pane draw records;
- the pane classification and the cursor rules (`pick`, `pick_extend`).

What changed is the plumbing:

- Offsets now come from `ktgl::Config` instead of constants.
- The per-frame navigation now runs in the engine. It is the same state machine with the same numbers.
- The `play_follow_live` gate now reaches the adapter as `Config::world_live`.

Hover candidates are passed as element flags:

| Selection mode | `kHover` | `kSlider` |
|---|---|---|
| Mode 2 (sibling button layouts) | the group's entries | none |
| Other modes | entries of the selected entry's layout, or of any layout when nothing is selected | the same layout rule |

Panes of a `menu_key_guides` layout carry their button as `click`. Config keys, log lines and the status text are the same as before; the keys stay in `[pkp]`.

## ui2d adapter (Donkey Kong Bananza)

Source: `game/dkb/dk-bananza-040074A01BF12000/updates/458752/exefs/main` as an ELF, `work/ida/dkb/main.elf` (IDA database `work/ida/dkb/main.elf.i64`, image base 0, 120800 functions after auto-analysis). The game keeps the C++ RTTI names of the `nn::ui2d` classes (`N2nn4ui2d4PaneE`, `...6LayoutE`, `...5PartsE`, `...8BoundingE`, `...TransitionConditionIsHitE` ...). Its own classes have no names.

### How the game handles the pointer

Bananza's menus are built from layout *controls*. A factory, `0x5b0d9c`, creates the control named in the layout by comparing strings. The names it knows: `UniteButton`, `NormalButton`, `DecisionButton`, `SelectButton`, `CheckButton`, `DragButton`, `CheckKeepButton`, `TwoTouchCheckKeepButton`, `BoxCursor`, `TraceGauge`, `ScrollIndicator`, `DragScrollButton`. Each button class's vtable slot 0 returns its name; `0x170c7ec` returns "NormalButton", for example.

The game already drives these buttons with a pointer (the touch screen). A *scene* runs `0x1709ce0` each frame:

1. It reads its pointer state: a 1280x720 position at +280/+284, trigger, release and hold bits 0x8000 at +8/+12/+16, and a valid bit at +20.
2. It converts the position to layout units with the layout's size: x = (px / 1280 - 0.5) * width, y = (py / 720 - 0.5) * (bottom - top).
3. It calls the button group's pointer update through the group's vtable (slot 2).

The scene's update `0x46880` inlines `0x1709ce0` when the vtable slot holds it (`0x46960`: `ADRL X10, sub_1709CE0`, compared with `[vtable+0x140]`). An entry hook there would miss those calls. The group update itself is always reached through its vtable: there is no direct `BL` to it. That makes it the hook.

### Addresses and offsets

Confidence: **high** means it was read directly from decompiled code. **Medium** means it is consistent across several functions, but its meaning is inferred. Nothing has run in game.

| Key | Value | Confidence | Evidence |
|---|---|---|---|
| `ui2d_group_update` | `0x16ffabc` | high | The only function in vtable `0x23dc2e0` (slot 2, `+0x10`). Called as `(*(grp->vt + 0x10))(grp, float pos[2], trig, rel, hold, 1)` from `0x1709ce0` (`0x1709e74..0x1709ea0`: `X1 = sp+8` holding the converted point; `W2/W3/W4 = UBFX bit 15` of state +8/+12/+16; `W5 = 1`). The same function calls it with `pos = 0` when there is no touch. Inside it: each button in the list that passes the ready mask goes through `button->vt+0x100` (IsHit, below). The result goes to `grp+40`, calling the old button's `vt+0x30` (Off) and the new one's `vt+0x28` (On). A trigger calls `vt+0x38` (Decide). |
| `ui2d_grp_buttons` | 8 | high | `0x16ffabc`: the list head at `grp+8`, last node at `grp+16`. A node is at `button+8`, since the code uses `node - 8` and the flags at `node+56` (= `button+64`). Also the creator `0x5e517c` sets `+8/+16` to self. |
| `ui2d_grp_current` | 40 | high | `0x16ffabc` stores the hit button in `grp+40` after the On / Off calls. |
| `ui2d_grp_flags`, `ui2d_grp_pointer_bit` | 64, `0x2` | high (offset), medium (meaning) | `0x5e517c` sets `+64 = 3`. In `0x16ffabc`, buttons are hit-tested only when `pos != 0 && (flags & 2)`. Bit 2 (`0x4`) picks the "decide on release" path (`TouchOn` / `TouchDecide`). |
| `ui2d_btn_layout` | 32 | high | `0x5b18c0` (the button setup, vtable slot 31) stores its third argument there. That argument's `+24` is the root pane searched by name, which is `nn::ui2d::Layout`'s root pane field. |
| `ui2d_btn_flags`, `ui2d_btn_ready_mask` | 64, `0x220010` | high (test), medium (meaning) | `0x16ffabc` hit-tests only buttons with `(~flags & 0x220010) == 0`. On (`0x6fc5c8`) needs `0x11` and Decide (`0x76cec0`) needs `0x14`. |
| `ui2d_btn_hit` | 88 | high | `0x5b18c0`: `button+88 = FindPaneByName(root, control pane "Hit")` (`0xfe20`). `button+96` is the "Cursor" pane and `button+80` the "Disable" / "Invalid" pane. |
| (IsHit) | `0x17004c0` | high | Button vtable slot 32 (`+0x100`). It tests the hit pane with `0x170e5e0(pos, pane)` and the visibility walk below. |
| `ui2d_lay_size` | 40 | high | `0x8522f4`, called by `0x1709ce0` on `scene+40`, reads the layout's `+40/+44` as width and height and returns the centred rectangle (±w/2, ±h/2). This matches `nn::ui2d::Layout`'s layout size after the root pane (+24) and the group container (+32). |
| `ui2d_pane_parent` | 24 | high | `0x17004c0` walks `pane = *(pane+24)` up to the root. |
| `ui2d_pane_flags`, `ui2d_pane_alpha` | 88, 89 | high | `0x17004c0`: the pane needs flag bit 0 and alpha ≠ 0. Each ancestor needs bit 0, and when it has bit 1 (alpha reaches the children) an alpha ≠ 0. This is `nn::ui2d::Pane`'s visible / influenced-alpha flags and alpha. |
| `ui2d_pane_size`, `ui2d_pane_base`, `ui2d_pane_mtx` | 80, 91, 112 | high | `0x170e670` (the hit box used by `0x170e5e0`) works as follows. Half sizes are `|w*m00|/2` and `|h*m11|/2`, with the size at +80/+84 and `m00` = +112, `m11` = +132. The centre starts at translation (+124, +140) and is moved by the base position byte +91: horizontal bits 0-1 (1 = left, so + half width; 2 = right, so − half width) and vertical bits 2-3 (1 = top, so − half height; 2 = bottom, so + half height). The matrix is the 3x4 global matrix at +112. |
| `ui2d_pane_name` | 176 | high | `0xfe20` (FindPaneByName) compares `pane+176..` byte by byte with the name, as an inline `char[]`. |
| Button On / Off / Decide | `vt+0x28` `0x6fc5c8`, `vt+0x30` `0x730a70`, `vt+0x38` `0x76ce90` | high (queue codes), medium (names) | They queue requests 0, 1 and 2 at `button+58..61` (count `+62`). `0x76ec38`, the button update, plays them as the `On`, `Off` and `Decide` animations; `0x17003f8` binds "On", "TouchOn", "Off", "TouchOff", "Decide" and "TouchDecide". The adapter does not call these. The game's own group update does. |

Names given in the IDA database: `ui2dctl_ButtonGroup_UpdatePointer`, `ui2dctl_Scene_UpdatePointer`, `ui2dctl_Button_IsHit`, `ui2dctl_IsContainPane`, `ui2dctl_PaneHitBox`, `ui2dctl_Button_Init`, `ui2dctl_CreateControl`, `ui2dctl_Button_RequestOn/Off`, `ui2dctl_Button_Decide`, `ui2dctl_Button_Update`, `nn_ui2d_Pane_FindPaneByName`, `ui2d_Layout_GetViewRect`, `ui2dctl_CreateButtonGroup`.

### What the adapter does

- **Entry hook on `ui2d_group_update`** (the game's thread). It records the group and the time.
- **While the menu pointer is up**, the hook also rewrites the call's arguments:
  - `x1`: the mouse on the UI plane times half the layout size, in layout units with y up;
  - `w2`: trigger, `w3`: release, `w4`: held, `w5`: 1.

  The game then hit-tests, turns buttons On and Off, and decides, as it does for touch.
- **Elements** (main thread). Every button of each pointer-enabled group updated in the last 100 ms (same vtable as when it was recorded), when it passes the ready mask and its Hit pane is visible by the game's rule. The rectangle is the Hit pane's box, scaled onto the 1920x1080 canvas by the layout's size. The selection is `grp+40`.
- **`focus`**: nothing to do. The fed position already moves the game's pointer focus. The engine waits for `grp+40`.
- **`confirm`**: the next update of the button's group gets a trigger (with hold), then the one after gets a release. Both are at the centre of the button's Hit box.
- **Gate** (`menu_in_game`): a pointer-enabled group with an eligible visible button counts as a menu. This is inferred: it assumes the game turns pointer input on for menu groups only.
- **No sliders.** The wheel keeps the menu bindings.

### Not known yet

- Whether Bananza's pad selection uses `grp+40` too, or a separate cursor (`BoxCursor`). If it is separate, hover and the pad focus could both show a highlight.
- Whether groups with bit `0x4` (decide on release) react to hover without a press. The decompiled path suggests they do not, so in those groups a click still works but hover may not.
- Whether `0x16ffabc` runs every frame for every open menu. The scene skips it when the group has no buttons (`0x46948`) or the scene is inactive.
- The game module (`games/dkb`) and `recomp/games/dkb` do not exist yet. Wiring them up means:
  - load a `games/dkb/menu.ini` with the values above into the config;
  - call `ui2d::config_from_ini("menu", &cfg)` and then `ui2d::install(base, cfg)`;
  - set the `GamePlay` menu functions;
  - add `entryhook 0x71016ffabc` to its `main.sites`.
