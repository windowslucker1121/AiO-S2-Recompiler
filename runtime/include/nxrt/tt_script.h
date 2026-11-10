// SPDX-License-Identifier: GPL-3.0-or-later
// Scripted-run DSL (S5): text format, parser and step list. The runner (tt_runner.cpp) executes the steps frame by frame.
// Grammar (one step per line, '#' starts a comment, blank lines ignored, keywords are case-insensitive, numbers decimal or 0x hex,
// floats decimal). A "frame" is one game frame boundary (tt.h). Waits count frames from the moment the step starts.
//
//   <n>                               wait n frames (a bare integer)
//   wait <n>                          same
//   <n>:<keys>[/<h>]                  smo-recomp style: wait n frames, then press <keys> for h frames (default 4); two steps: Wait, Press
//   press <keys> [for <n>]            hold the buttons for n frames (default 4), release them, go on. Blocks for n frames.
//   hold <keys>                       press the buttons and keep them down (until release); goes on at once
//   release <keys>|all                release some or all held buttons; goes on at once
//   key <name> [for <n>]              press the named keyboard key / mouse button / wheel notch for n frames (default 4), release, go on
//                                     after n frames (blocks like press)
//   keydown <name>                    press the named input and keep it down; goes on at once
//   keyup <name>                      release the named input; goes on at once
//                                     <name> is the rest of the line up to an optional trailing " for <n>" (case-insensitive): "W",
//                                     "Left Ctrl", "Page Up", "Mouse1", "WheelUp". The name is not checked here; the input layer
//                                     (tt.h set_key_sink) decides, and an unknown name fails the run.
//   stick left|right|l|r <x> <y> [for <n>]   stick vector, each -1.0..1.0 (y up = +). With `for n` it blocks n frames and then returns to 0;
//                                     without it the vector persists until the next stick step for that stick
//   mouse <dx> <dy> [for <n>]         raw mouse counts delivered once per frame for n frames (default 1); blocks for n frames
//   @frame <n>                        wait until the global frame counter is >= n
//   @area <id> [timeout=<n>]          wait until the current area id equals <id> (GUID text 8-4-4-4-12, or a name from the region table)
//   @world <id> [timeout=<n>]         the same for the world id
//   @pos <x> <y> <z> [<r>] [timeout=<n>]   wait until the player is within r (default 1.0) world units of the point
//   @igt <seconds> [timeout=<n>]      wait until the in-game play time is >= seconds
//   @run <seconds> [timeout=<n>] [soft]      wait until the in-game play time has advanced by <seconds> since this step began (the game clock stands
//                                     still while an area loads, so this is "the game ran for that long"). `soft`: when the timeout runs out,
//                                     log "SOFT-TIMEOUT" and go on instead of failing the run
//   @mash <keys> <period_ms> area|world <id> [timeout=<ms>]
//                                     press the buttons for 250 ms of host time every <period_ms> until the area (or world) becomes current: gets
//                                     through the front end without knowing how long each menu and load takes. Timeout in host ms, default 600000
//   @give <item> <amount>             set an item of the player state (CPlayerStateMP1 item table): amount and capacity. <item> is a number or the game's
//                                     name without case or underscores (MorphBall, ChargeBeam, Missiles, XRayVisor, WaveBeam ...)
//   @rate <hz>                        the game step is exactly 1/hz for every frame from here on (hz > 0), 0 = the configured mode again; the clock
//                                     itself stays real, so areas load at full speed
//   @sleep <ms>                       wait <ms> of host wall-clock time (not game time: loading and fade-outs run on the host clock even in
//                                     deterministic mode); frames keep being delivered meanwhile
//   @warp <world> <area>              warp to the area (practice-mod sequence, done by the runner); then waits until that area is current
//   @shot <name>                      save a screenshot <shot_dir>/<name>.png at this frame (does not block)
//   @hash [<label>]                   write a labelled hash line into the trace and the result file (does not block)
//   @log <text...>                    write the rest of the line to the log
//   @assert <field> <op> <value>      fail the run now if false. fields: area world frame igt health px py pz vx vy vz speed state tod prog (tod: the in-game time of day, hours; prog: the game's progress counter).
//                                     ops: == != < <= > >=. area/world compare ids (== and != only); the others compare numbers
//   @goto <x> <z> [<r>] [soft] [timeout=<n>]   walk to (x, z) (radius r, default 60): the left stick is steered each frame from the player's
//                                     position and velocity (closed loop, so it works with a camera-relative stick whatever the camera does);
//                                     [test] goto_mirror = true for games whose stick-right turns the heading the other way. soft: a timeout
//                                     logs and the script goes on
//   @mashuntil <keys> every <n> until <field> <op> <value> [timeout=<n>]   press the buttons for 4 frames every n frames until the condition
//                                     (same fields and operators as @assert) holds, e.g. `@mashuntil A every 120 until state >= 0` drives menus
//   @watch on [vely|posy|both] | off    hardware write watch (debug registers) on the player's physics velocity.y and/or transform position.y; every write is logged with
//                                     the writing instruction, the frame and the value (lines `watch:` in the log); off stops it and logs how many hits were seen
//   @objs <class>...                  list every live object of the named classes with its position (door dock pickup special actor platform trigger player, or a main-module
//                                     vtable offset): a memory scan for the vtable pointer; result in <out>/objs.txt, one `class x y z addr` line per object
//   @rays <in> <out>                  batch of ray casts against the static collision of the loaded areas (the game's own NCollisionMP1::RayStaticIntersection): in = lines `x y z dx dy dz len`
//                                     (dir is a unit vector), out = lines `hit t px py pz nx ny nz` (hit 0/1). Runs in the frame hook, the game stands still meanwhile
//   @probe <in> <out>                 the player's own collision box against the static world at positions: in = lines `x y z`, out = `0` free / `1` collides per line
//   @poke <in>                        write guest memory: lines `addr [off ...] : hexbytes` (pointers followed as in @peek; a token `+N` adds N without following); a development aid, e.g. to restore health
//   @peek <in> <out>                  read guest memory: in = lines `addr len [off ...]` (hex addr; each off follows a 64-bit pointer first), out = one hex line per request (`-` on failure)
//   @timeout <n>                      default timeout (frames) of every later wait; initial 3600; 0 = none
//   @pass | @fail [<message...>]      end the run (exit code 0 or 1) at once
//   @end                              the same as @pass
//
// <keys>: button names joined with '+': A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK (LSTICK/RSTICK = the stick clicks),
//         also START = PLUS, SELECT = MINUS. Names are case-insensitive. Same bit numbers as nn::hid::NpadButton.
// A wait with a timeout that runs out fails the run (exit code 1) with a message naming the script line.
// A script that reaches its last step passes unless an @fail/@assert failed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::tt {

enum class StepKind : uint8_t {
    Wait,        // count frames
    Press,       // buttons for count frames, then release
    Hold,        // buttons
    Release,     // buttons (all = every held button)
    Stick,       // which (0 left 1 right), x, y; count > 0: blocks count frames then zero; count == 0: persists
    Mouse,       // x, y raw counts per frame for count frames
    WaitFrame,   // count = target frame
    WaitArea,    // id (16 bytes), timeout
    WaitWorld,   // id (16 bytes), timeout
    WaitPos,     // x, y, z, r, timeout
    WaitIgt,     // x = seconds, timeout
    WaitMs,      // x = milliseconds of host time
    Give,        // x = item id, y = amount
    Rate,        // x = hz (0 = real clock)
    Mash,        // buttons every x ms until area (text "area") / world (text "world") id; timeout in host ms (-1 = 600000)
    WaitRun,     // x = seconds of in-game play time from the step start, timeout
    Warp,        // id = area, id2 = world
    Shot,        // text = name
    Hash,        // text = label (may be empty)
    Log,         // text
    Assert,      // field, op, text = value text, x = numeric value, id = id value
    Timeout,     // count
    Pass,
    Fail,        // text = message
    Key,         // text = input name, count = frames (-1 = not given: the runner uses 4); blocks like Press
    KeyDown,     // text = input name
    KeyUp,       // text = input name
    Grant,       // text = item name (or 'all'), count = item id (-1 for all), x = amount, y = capacity (written into the player state's power-up array)
    Call,        // count = main-module offset of a guest function, text = its arguments (numbers or $gs $ps $mgr $player), run in the frame hook
    Objs,        // text = class names (door dock pickup special actor platform trigger, or a vtable offset): scan memory for instances, write <out>/objs.txt
    Watch,       // text = on|off|dump, y = 1 velocity.y, 2 position.y, 3 both (hardware write watch on the player, see the game module's probes)
    Probe,       // text = "IN OUT": does the player's collision box collide with the static world at these positions (S26)
    Poke,        // text = in-file: write guest memory (S26, development aid)
    Peek,        // text = "IN OUT": read guest memory (S26)
    Rays,        // text = "IN OUT": batch of static-collision ray casts (S26)
    PressUntil,  // buttons pressed 4 frames every `count` frames until the condition (field/op/x/id, as Assert) holds; timeout
    GoTo,        // x, z, r: steer the left stick until the player is within r of (x, z); soft = a timeout logs and goes on
};

enum class AssertField : uint8_t { Area, World, Frame, Igt, Health, PosX, PosY, PosZ, VelX, VelY, VelZ, Speed, State, Tod, Prog };
enum class AssertOp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

struct Step {
    StepKind kind = StepKind::Wait;
    int line = 0;                  // 1-based source line
    std::string src;               // the source line, trimmed (for messages)
    uint64_t buttons = 0;          // Press/Hold/Release: bit mask (bit n = 1 << NpadButton n); Release all: ~0ull
    int64_t count = 0;             // frames; -1 = "not given" for Press (the runner uses 4) and Mouse (1)
    int64_t timeout = -1;          // frames; -1 = use the script default; 0 = none
    int which = 0;                 // Stick: 0 left, 1 right
    double x = 0, y = 0, z = 0, r = 0;
    uint8_t id[16] = {};           // Area/World/Assert(area, world)/Warp (the area): the resolved id in memory order (see tt_guid_to_bytes)
    uint8_t id2[16] = {};          // Warp: the world id
    std::string name1, name2;      // the id/name text as written (Warp: name1 = world, name2 = area)
    bool soft = false;             // PlayTime: a timeout logs and continues instead of failing
    std::string text;
    AssertField field = AssertField::Frame;
    AssertOp op = AssertOp::Eq;
};

struct Script {
    std::vector<Step> steps;
    std::string path;
};

// Name -> (world id, area id) table. File format (UTF-8, '#' comments): `name <TAB> world-guid <TAB> area-guid` per line; an area
// name is unique ("frigate_cargo_freight_lift") and a world entry has an empty area column ("frigate\t<guid>\t").
struct RegionTable {
    struct Entry { std::string name; uint8_t world[16]; uint8_t area[16]; bool has_area; };
    std::vector<Entry> entries;
    bool load(const std::string& path, std::string* err);
    bool load_text(const std::string& text, std::string* err);
    const Entry* find(const std::string& name) const;     // case-insensitive
};

// GUID text "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> the 16 bytes as the game's CObjectId holds them in memory.
// VERIFIED against the real game under Eden (docs/S5.md): a Windows-style GUID struct, the first three groups little-endian.
// The layout lives only in these two functions.
bool tt_guid_to_bytes(const std::string& text, uint8_t out[16]);
std::string tt_guid_to_text(const uint8_t in[16]);
std::string tt_guid_hex(const uint8_t in[16]);        // raw: 32 hex digits in memory order

// Parse. `regions` may be null (then only GUID text is accepted as an id). On error returns false and *err =
// "<path>:<line>: message" (the first error). `path` is only used for messages.
bool parse_script(const std::string& text, const std::string& path, const RegionTable* regions, Script* out, std::string* err);
bool parse_script_file(const std::string& path, const RegionTable* regions, Script* out, std::string* err);

}  // namespace nxrt::tt
