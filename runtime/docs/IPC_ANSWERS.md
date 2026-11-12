# Deliberate answers for commands title A sends (no stub_result needed)

`[ipc] stub_result` / `NXRT_IPC_STUB_RESULT` defaults to `kStubService` (0xBCBD5): an unknown command fails and is logged at Warn
once per (service, command), with counts in the end-of-run summary. Every command that reached the stub path in the title A run logs
(play/ and work/run*/, Nov 2026), plus the ones work/audit/hle.md flags, now has its own answer. Layouts come from Switchbrew where it
has the id, otherwise from the SDK client proxy (address given).

| Service / interface | Cmd | Answer | Why / source |
|---|---|---|---|
| `ssl:s` | all | same ISslService as `ssl` (CreateContext returns a real context; 5 SetInterfaceVersion ok) | Platform 2 name of ssl; was a StubService |
| nn::hid::IHidServer | 3110-3113, 3130-3133, 3135-3143 | Platform 2 controller mouse (Nmouse / Jcms), none returns data except 3133. 3139 / 3130 InitializeNmouse (both forms) and 3110 InitializeJcms keep the transfer memory, list a merged mouse and a platform 2 controller (R) mouse and the sampler writes them (pointer, pointer range, wheel, buttons, attributes bit 1, sampling numbers); 3111 / 3131 drop it; 3112 / 3113 Start/StopJcms accepted (the rings always sample); 3132 sets and 3133 returns the pointer range; 3135 EnableShowingCursor and 3141 EnableNmousePointer write the byte the client reads back; 3136-3138 stored; 3140 SetNmousePointerPosition logged (the PC mouse is not moved); 3142 RestrictNmousePointer and 3143 (unknown) accepted. Title A sends 3110-3112, 3131, 3136-3139 | sdk proxy thunks 0x75006701b4..0x75006702ec (vtable slots 0x538..0x5B8) matched to the nn::hid::detail callers; layouts from the thunks' CMIF writers; `docs/hid_mouse_sw2.md` |
| nn::hid::IHidServer | 214 | SendVibrationValues (platform 2 form, extra u16 = 2): stored and forwarded to the pad rumble like 206 | sdk nn::hid::detail::SendVibrationValues 0x750064d360 (vtable slot 0x2f0) |
| IApplicationFunctions | 320 | success, 2 zero bytes | sdk proxy 0x75001bd9f8 reads two u8; name unknown |
| IApplicationFunctions | 321 | success, 1 zero byte | sdk proxy 0x75001bda04 reads one u8; name unknown |
| ILibraryAppletAccessor | 90 | success, no output (input bool) | sdk proxy 0x75001bd560; name unknown |
| ILibraryAppletAccessor | 170 | success, a copy handle of an event that never fires | platform 2; name unknown. oot's sdk creates every applet through 0x75001e37f0, which attaches this handle as a system event next to GetAppletStateChangedEvent's and aborts when the command fails: the first in-game controller applet (2026-11-12) crashed there. Thunk 0x75001d03dc, same reply shape as 105. Command 160 beside it (thunk 0x75001d03d0) is still unanswered; not seen called |
| nn::hid::IHidServer | 107 DisconnectNpad | a pad the controller applet gave to player 2.. goes back to player 1 | a game that wants fewer players disconnects the others; ignoring it while a pad was assigned made oot ask for the controller applet again every frame |
| ILibraryAppletCreator | CreateLibraryApplet(Ex), inline swkbd | refused with 128-712 (was 128-35) | the SDK fails gracefully only on am 701..749; 128-35 made it svcBreak |
| stpl:u IUserService | 1 GetRequiredMemorySize | success: type 0 = 1 page + guests x 3 pages, type 1 = guests x 1 page | SDK aborts on failure (0x75006ad634). Was 0: nn::streamplay::Initialize divides the buffer size by the per-guest size (CalculateGuestCountFromMemorySize, sdk 0x75006b9850) and aborts below 1, so hosting GameShare aborted |
| stpl:u IUserService | 9 GetWorkBufferSizes | success, 4 x u64 one page (global + three per-guest parts) | type 0 carves its transfer memories from the game's buffer with these (CreateWorkBufferInProcessGlobal / PerGuest); real sizes unknown, nothing is ever streamed |
| stpl:u IUserService | 300 GetHidSharedMemory | success, {size 0x84000, offset 0} + zeroed shared memory | SDK aborts on failure (0x75006b9744). 0x84000 is how far HidSharedMemoryAccessor reads (guest pads, touch screens, Jcms; layout in misc.cpp); all zero = no guest pad (style set 0) and empty rings, i.e. no GameShare session |
| stpl:u IUserService | 2 Initialize, 3 Finalize, 5 CloseSession, 7, 8, 25, 27, 502-507, 509, 510 | the GameShare host path up to the matchmaking applet: Initialize keeps (type, mode); 7 an event that never fires; 8 all zero (not connected); 502 / 503 answer that type / mode; 504 0x80 zero bytes (the applet argument); the rest accepted | Title A main 0x7101132f60: Initialize (state 1), then OpenSessionWithMatchmakeSupport (state 2). Failures in 502 / 503 / 505 / 506 abort the SDK; a failure in Initialize went to the error applet. With these the matchmaking applet runs and ends cancelled: 0x3D1F, the game backs out quietly and calls Finalize. Ids from the proxy vtable 0x7500dd8d20 (slot +32 = cmd 1); layouts in misc.cpp |
| stpl:u IUserService | others (StartStreaming 4 / 500 / 501, guest queries) | not implemented (fails, logged) | only reached with a connected guest, which never happens here |
| IApplicationFunctions | 140 GetFriendInvitationStorageChannelEvent | success, an event that never fires | nn::friends::GetFriendInvitationNotificationEvent (sdk 0x75003d8968, vtable slot 0x1B0) aborts on failure; offline, no invitations arrive |
| IApplicationFunctions | 141 TryPopFromFriendInvitationStorageChannel | 128-2 (no data) | nn::friends::TryPopFriendInvitationNotificationInfo (0x75003d89f0, slot 0x1B8) reads 128-2 as "none" and returns false; any other failure aborts |
| ILibraryAppletCreator / accessor | MyPage applet 0x100001A (friend invitation) | created; Start ends it at once, GetResult 128-22 (cancelled) | nn::friends::StartFriendInvitation / StartSendingFriendInvitation (0x75003e32a0, 0x75003e33b0) run it with arg kind 8 / 9; creating must succeed (CreateMyPageApplet aborts otherwise). Cancelled makes the friends call return 121-30 (0x3C79): the game ignores it for StartFriendInvitation (main 0x7101b2a5a0) and takes its failure branch for StartSending (main 0x7101b2a7ec). Offline: no invitation sent. The friends service itself gets no invitation IPC from these calls |
| IManagerForApplication | 3 LoadIdTokenCacheDeprecated, 4 LoadIdTokenCache | 124-200 NetworkServiceAccountRegistrationRequired | no NSA token; SDK wrapper returns it to the game |
| IManagerForApplication | 170 LoadNetworkServiceLicenseKindAsync | async context: done, GetResult fails (110-300) | offline, no online-service licence |
| IAsyncNetworkServiceLicenseKindContext | 100 GetNetworkServiceLicenseKind | success, u32 0 (none) | SDK aborts if it fails (0x750018c634) |

## Library applets title A can launch

Found by walking the SDK (sdk 0x7500000000) up from `nn::applet::CreateLibraryApplet` / `nn::la::StartLibraryAppletEasy` to the
functions main imports (14 imports reach it; subsdk0 imports none). Each launch logs `am: library applet 0x... started (...)` or its own
line. "Cancelled" = Start ends the applet at once and GetResult answers 128-22 (exit reason Canceled), with no output.

| Applet id | Game reaches it through (main import) | Answer | What the game then does |
|---|---|---|---|
| 0x1020011 swkbd | `nn::swkbd::ShowKeyboard` (main 0x7101141f00, 0x7101142190) | the PC prompt's text or `[applet] swkbd_text`; text check round trip | takes the name |
| 0x100000E error | `nn::err::ShowError(Result)`, `ShowApplicationError`; also `ldn::Connect`, `pctl::TryBeginFreeCommunication`, GameShare failures via ShowError | logged (code, messages), ends with success, no UI | ShowError ignores the result |
| 0x1000010 player select, mode 0 | not imported directly (kept for completeness) | the one user, result 0 | — |
| 0x1000010 player select, mode 2 / 10 / 11 | `nn::account::EnsureNetworkServiceAccountAvailable` (12 call sites, e.g. main 0x7100542f20, 0x7100583860), `ShowLicenseRequirementsForNetworkService` (0x7100543098), `err::ShowError` for some network results | output result 0x27C (124-1 CancelledByUser), no user | the callers check 0x27C and stop quietly. Was result 0 before: mode 2 then made them retry the NSA token request in a loop, since acc keeps answering 124-200 |
| 0x1000014 shop | `nn::ec::ShowShopAddOnContentList` (main 0x7100fd2620) | cancelled | ShowShopPage returns 0x690 (closed), which ShowShopAddOnContentListImpl accepts |
| 0x100001A MyPage | `nn::friends::StartFriendInvitation`, `StartSendingFriendInvitation` | cancelled (see row above) | ignored / failure branch |
| 0x100000D data erase | `nn::fs::EnsureSaveData`, `ExtendSaveData`, only when IApplicationFunctions 20/25 report a required size above 0 (they answer 0 here, so not reached) | cancelled | fs returns 0x3E02 (not enough space) |
| 0x0F net connect | `nn::nifm::HandleNetworkRequestErrorResult` | both imports are replaced in core/switch2.cpp (not resolved) before the SDK runs; cancelled if reached | stays offline |
| 0x1000024 / 0x1030024 GameShare matchmaking | `nn::streamplay::OpenSessionWithMatchmakeSupport` (main 0x7101132f60) | cancelled | the SDK returns 0x3D1F, which main 0x7101133704 treats as backing out (no error). Reached once stpl:u answers the host path (see the stpl:u rows) |
| any other id | not launchable from title A | ends at once, no output, GetResult success; logged at Warn and counted as a gap (see Policies below) | — |

Not launchable from title A (no import reaches them): controller support / strap guide / firmware update (nn::hid), web, offline web,
album / photo viewer, Mii edit, amiibo settings, parental controls (nn::la::auth), chat, save data backup, inline (modeless) keyboard.
The inline keyboard is still refused with 128-712.

## Policies (`[applet] <key> = auto | cancel | prompt`, since 2026-11-12)

Every applet above has a row in the table of `am.cpp` (`kApplets`) with an `[applet]` key and a default policy (runtime/docs/CONFIG.md).
`prompt` uses a native window (`src/services/applet_ui.cpp`) only while the play layer has a game window. New answers:

| Applet id | Key | Default | Answer |
|---|---|---|---|
| 0x0C controller support | `controller` | prompt | mode 0: ResultInfo {s8 players, u32 npad id 0, u32 result 0} with the players connected (player 1 = keyboard and mouse + first pad, then one per pad), at most the game's max; pads 2.. become players 2.. (`hid::set_player_pads`, play.cpp `poll_pads`). Fewer than the game's min with prompt: a dialog listing the pads (OK when enough, Continue anyway, Cancel = the players there are). Modes 1.. (strap guide, firmware update, key remapping): success at once. `cancel`: result word 1 (the SDK returns 0x183ACA) |
| 0x13 web, 0x17 offline web, 0x18 login share, 0x19 Wi-Fi web auth | `web` | prompt | cancelled (128-22): nn::web::ShowWebPage returns 0x2D2 instead of aborting for a missing 0x2000-byte return value; prompt asks first whether to open the initial URL (TLV type 1) in the browser |
| 0x0B amiibo settings | `cabinet` | auto | cancelled (the SDK answers its nfp "cancelled" result either way) |
| 0x0E error | `error` | prompt | logged; prompt adds a non-modal notice (same code at most once a minute) |
| 0x0F net connect, `nn::nifm::HandleNetworkRequestErrorResult` | `net_connect` | prompt | not resolved; prompt adds a notice once per run with a button to the Windows network settings |
| unknown ids | `other` | auto | ends at once, no output, success; counted in gaps.tsv (`applet`) |

## Library applets oot can launch

From oot's main imports (the 1,183 imports demangled; callers in the lifted main). It imports no keyboard, shop, friends, album, Mii,
chat, GameShare or local-play function.

| Applet | Import (main stub) / call sites | What oot does with the answer |
|---|---|---|
| controller support 0x100000C | `nn::hid::ShowControllerSupport` (0x7103c447d0); 0x7103b25af0 / 0x7103b25b48 in the per-frame controller manager 0x7103b25780, 0x7103b26118 | result 0: re-binds to the selected id (0x20 = handheld). Cancelled (0x183ACA): asks again at once, in a loop: never answer cancelled by default. Triggered only when a wanted player's pad is missing; player 1 is always connected here |
| web page 0x1000013 | `nn::web::ShowWebPage` (0x7103c42170); 0x71032c31c0 in the online flow (an online-service info page) | needs a successful online login first: not reached offline. Cancelled lets the caller go on |
| error 0x100000E | `nn::err::ShowError(Result)`; 0x7102893d94, 0x7102893e34, 0x71032c2e98, 0x7103b63ee4, 0x7103b656d8 (network flows, amiibo) | ignores the result |
| net connect | `nn::nifm::HandleNetworkRequestErrorResult`; 0x7102893f30, 0x71032c2f74 | replaced in core/switch2.cpp; the flow fails, the game stays offline |
| player select mode 2 | `nn::account::EnsureNetworkServiceAccountAvailable`; 0x7103238c54 | 0x27C: returns 3 (failed), no retry |
| amiibo settings 0x100000B | `nn::nfp::StartNicknameAndOwnerSettings` / `StartRestorer` / `StartGameDataEraser` | needs a detected tag; nfp reports no device, so not reached |

Also on the online path: `nn::pctl::CheckAgcPermissionUnderMinimumAge(Uid)` (sdk 0x75005a2270, proxy slot +432 = command 1101), whose SDK
wrapper aborts on failure: pctl answers it with "permitted" (network.cpp).
