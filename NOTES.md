# sonic_allstars_nx: porting notes

Technical notes on how the 32-bit Android build of Sonic & SEGA All-Stars
Racing runs on the Switch, what the 32-bit libraries needed, and what may help
anyone porting another 32-bit game. File and function names refer to this
repository unless stated otherwise.

## The setup in short

* The game is `com.sega.ssasr` 1.0.1 (versionCode 20), armeabi. All of its
  code is one library, `lib/armeabi/libssasr.so`: Sumo Digital's engine and
  the game, Distinctive Developments' Android layer, OpenAL Soft 1.13 and
  zlib, built with GCC 4.4.3 for ARMv5TE, soft-float.
* Its data is one archive, `packres.png` (a "D0" pack), stored inside the
  expansion file `main.20.com.sega.ssasr.obb`. The music is MP3 files in the
  APK. The intro is an MP4 in the OBB.
* The wrapper is an AArch32 program built with devkitARM and
  [libnx32](https://github.com/aks796/libnx32) (the AArch32 libnx), linked as
  a PIE ExeFS NSP. Graphics use [mesa32](https://github.com/aks796/mesa32)
  (Mesa and libdrm_nouveau); video and music use
  [ffmpeg32](https://github.com/aks796/ffmpeg32).
* A 32-bit program cannot be an NRO (hbloader is 64-bit). The launcher,
  `sonic_allstars_nx.nro` (64-bit, devkitA64), carries the NSP in its RomFS
  and installs it as the Atmosphère ExeFS override of the sphaira forwarder
  it was started from (`launcher/source/main.c`, `source/dcr_exefs.h`).
  Later NROs update the override in place (`source/dcr_setup.c`).
* First-run setup and updates show a progress screen on the boot console
  (the game's name, a bar, the current step: `log_console_progress` in
  `source/util.c`, staged in `source/dcr_setup.c`). A normal start shows
  nothing. The console is closed for good before EGL takes the window (Mesa
  registers 3 buffer slots, the console 2; the console's third frame after
  Mesa fails with 0x2B59).

## The 32-bit libraries: fixed and still open

This port was first built against an earlier libnx32 and worked around the
problems below in its own code. libnx32 `master` branch, commit
`c6c53d20` (version 4.12.0, `41b61f92`), fixes most of them in the library.
This port is now built against that version. It keeps its own workarounds,
which were tested on hardware; they do not conflict with the fixes.

### [libnx32](https://github.com/aks796/libnx32): fixed in c6c53d20

* **svcSetThreadCoreMask** takes a `u64` mask. It used to be declared `u32`,
  so r3 (the mask's high half) held garbage, the kernel returned
  InvalidCoreId and every thread stayed on core 0; libnx's `pthread_create`
  failed as a result. This port still wraps it
  (`-Wl,--wrap=svcSetThreadCoreMask`, `source/dcr_sched.c`, now with a `u64`
  mask).
* **svcGetThreadCoreMask** stack imbalance.
* **svcWaitForAddress / svcSignalToAddress** stubs added (value int32 in r2,
  timeout r3:r4: the layout that worked on hardware and Ryujinx).
  `source/bionic_pthread.c` still issues its own and tests both layouts at
  boot.
* **armICacheInvalidate** is real on AArch32 (it was `(void)0`). This port
  still uses its permission-flip page (`source/code_flush.c`).
* **__libnx_initheap** is clamped to the 1 GiB heap region. This port
  overrides it anyway (`source/nx_init.c`).
* **audout/audin** use the 64-bit buffer descriptor and u64 tags. This port
  still sends its own commands (`source/ssr_audio.c`).
* **switch32.ld** places `.rel.dyn`. This port uses its own `dcr32.ld`.
* Also added: `envAcquireOwnProcessHandle()`, `nwindowGetDefaultDisplay()`, an
  AArch32 `__libnx_exception_entry`, SHA-1/SHA-256/HMAC, fsdev 0xE02 as
  EBUSY, a weak `timespec_get`, virtmem fixes for 32-bit.

### libnx32: still open

* **__nx_dynamic.** devkitARM's newlib, libsysbase and libstdc++ are not built
  `-fPIC`, so a PIE link carries R_ARM_RELATIVE relocations inside `.text`
  and `.rodata`. The stock crt0 cannot apply them: making a Code page writable
  turns it into CodeData, which can never be executable again (svcBreak
  0xDC03 on hardware). `source/crt0_reloc.c` replaces `__nx_dynamic`: it gets a
  real handle to its own process, maps each code block to a writable alias
  with `svcMapProcessMemory` (one kernel memory block per call; mapping
  `.text` and `.rodata` together fails with 0xD401), patches through the
  alias and unmaps it. Emulators refuse the pseudo-handle and do not enforce
  permissions; that case writes directly.
* **__appInit** aborts on any service failure. Under Ryujinx the time
  service's shared memory can fail to map in a 32-bit process
  (`MapSharedMemory` = InvalidCurrentMemory). `source/nx_init.c` records the
  failure and continues.
* **Enum sizes in IPC structures.** devkitARM uses short enums. Any IPC
  structure or argument typed as an enum gets the wrong size. libnx32 has
  fixes for the ones found (among them the supported-controller list);
  other structures should use fixed-width types.
* **CondVar.** Not a bug, but different from bionic: libnx's CondVar forgets
  a signal sent while nobody waits. Games that signal without holding the
  mutex can hang. `source/bionic_pthread.c` implements condvars as counters on
  the address arbiter.
* With the core-mask fix, libnx's `pthread_create` (and so Mesa's worker
  threads) now start. The game's own threads use this port's
  `source/bionic_pthread.c`, not libnx's.

### newlib (devkitARM)

* libm is a soft-float build: every double operation is a libgcc call.
  `source/bionic_math.c` does sqrt, fabs, rounding, min and max in VFP.
* `stat()` opens the file to read its size; `access()` is unreliable over
  fsdev; any filesystem result it cannot map becomes EIO
  (`source/bionic_io.c`).
* `mbstate_t` is 8 bytes; bionic's is 4 (`source/bionic_wchar.c`).
* errno numbers differ from Linux (`source/bionic_core.c`). `timespec`,
  `timeval` and `off_t` differ from bionic's 32-bit layouts
  (`source/bionic_time.c`, `source/bionic_io.c`).

### Mesa for AArch32 ([mesa32](https://github.com/aks796/mesa32))

* Short enums: `mesa_format` as a 16-bit enum crashed
  `st_choose_matching_format` on the first textured draw. Fixed in mesa32
  (099a02a3, "AArch32: don't depend on int-sized enums"). Mesa, and
  everything linked against it, has to agree on enum size.
* mesa32 also fixes `thrd_success` in `u_thread.h` (24aa14fe),
  `eglQuerySurface` sizes (4e41d89f), ETC2/ASTC on chipset 0x120 (2c27955c),
  render-to-texture without storage (dddc69a4), and adds opt-in glthread
  (972de9c1). This port links that build (`portlibs32/lib`).
* Still open: EGL pbuffers; the Switch EGL platform has window surfaces only
  and RGBA8888 configs without MSAA (`source/gl_mesa.c` drops attributes Mesa
  rejects); the console and EGL share the display's buffer slots.
* Mesa's generated code uses VCVT between float and fixed point. Ryujinx
  1.1.1098's A32 decoder does not have it (UndefinedInstructionException).
  `source/emu_fixups.c` rewrites those instructions, on the emulator only.

### FFmpeg for AArch32

* Built with `-fno-short-enums`. The files that include its headers
  (`source/ssr_media.c`, `source/ssr_video.c`) are compiled the same way
  (`Makefile`), and the interface between them and the rest has no enums.

### Toolchain

* The game's ABI is softfp: float arguments in core registers, VFP inside.
  The wrapper is built with `-mfloat-abi=softfp -mfpu=neon-fp-armv8`, like
  libnx32 and newlib, so no call needs `pcs` attributes.
* `-mtp=soft`, `-fPIE`, `-ftls-model=local-exec`.

## What was specific to this game

### Loading

* `source/so_util.c` loads `libssasr.so` into a reserved 16 MB region,
  applies its relocations in a staging copy and maps it. One relocation
  patches code; that is fine in the staging copy.
* Imports: 187, bound by name (`source/imports.c`, generated by
  `tools/gen_imports.py`): 126 to shims, 58 `gl*` to Mesa, 3 weak EHABI hooks
  left NULL.
* libgcc's `__sync_*` functions call the Linux kernel user helpers at
  0xffff0fc0 and 0xffff0fa0 (43 and 4 literals). Horizon has nothing there;
  the literals are pointed at `source/kuser.S` (`source/ssr_loader.c`).
* The library's 160 constructors run, then its `JNI_OnLoad` (OpenAL Soft's).

### Android and Java

* `source/jni_core.c` is a JNIEnv/JavaVM with no VM behind it.
  `source/ssr_java.c` answers the ~70 Java callbacks the engine makes (saves,
  music, text drawing, vibration; the store, Play Games and SEGA ID answer
  "not available").
* `source/ssr_boot.c` calls the engine's natives in the order the Android
  activity does, including the splash screens, pause, resume and exit.
* Text the engine draws through Java is drawn with the console's shared
  fonts (`source/ssr_text.c`, `source/ssr_font.c`).

### Files

* The APK is read in place (`source/ssr_apk.c`), with a RAM block cache
  because the engine reads it in many small pieces (`source/dcr_apkcache.c`).
* `packres.png` is read in place from the OBB, from a zip that stores the
  OBB, or from an APK that carries it (`source/ssr_pack.h`). Nothing is
  unpacked except the library.
* The APK and the data are found by their contents, not their names
  (`ssr_find_apk`, `ssr_find_data` in `source/ssr_pack.h`).
* Android paths are mapped onto the game folder (`source/dcr_path.c`).

### Graphics

* OpenGL ES 1.x through Mesa nouveau. 1080p docked, 720p handheld.
* The engine's float maths is libgcc software emulation (ARMv5TE,
  soft-float). Replacing 13 libgcc helpers with VFP code at load time
  (`source/ssr_patch.c`, `source/ssr_vfp.S`) is what makes 60 fps possible.
* The Android layer (`DDGLRefresh`) keeps a shadow of every GL buffer and
  texture for context loss, and finds buffers by linear search. The lookups
  are cached, the shadow copies of per-frame uploads skipped, and
  `ddGLDeleteBuffers` replaced with a version that ignores unknown buffers
  (the original writes before its table).
* The logic clock counts display refreshes instead of milliseconds, so frames
  never get two logic steps or none (`GetCurrentTime` hook).
* Anisotropic filtering and a smaller mipmap bias are added to the engine's
  textures.

### Sound

* OpenAL Soft (inside the game) writes to an Android `AudioTrack`. Here that
  is a C class (`source/ssr_audio.c`) that resamples to 48 kHz and feeds a
  ring; a mixer thread adds the music and submits to audout.
* The music is the APK's MP3 files, decoded by FFmpeg (`source/ssr_media.c`).
* The intro is decoded by FFmpeg and drawn over the game
  (`source/ssr_video.c`).

### Input

* Races: the human controller's `Update` is replaced; the pad goes straight
  to `STRacer::SetControls` each logic tick (`source/ssr_race.c`).
* Menus: the game has only touch controls. The wrapper keeps a focus on the
  game's own buttons (their rectangles from the engine's hit test) and sends
  touches to them (`source/ssr_menu.c`).
* The controller applet for two players returns 0x5D59 in handheld mode
  unless the Joy-Con hold type is set to horizontal before it is shown
  (`source/ssr_input.c`).

### Threads and memory

* Horizon does not preempt threads of equal priority on a core; Android
  does. Guest threads run at priority 59 on cores 0-2, where Mesosphère
  time-slices every 10 ms (`source/dcr_sched.c`).
* The heap is 1 GiB (the 32-bit heap region).

### Split screen

* Player 2 runs in a second copy of the engine: the library loaded twice,
  each with its own globals, saves (`data/p2`) and EGL context. Every call
  into the second copy must be made with that copy's globals selected
  (`AS_ENGINE`) and its GL context current; on hardware the two contexts do
  not share textures.
* Grand Prix and single races are one race in player 1's copy with player 2
  as a second human racer: a second camera, the frame drawn once per half,
  player 2's HUD drawn by player 2's copy (`source/ssr_split_race.c`).
* Battle and the two-player race use the game's LAN mode, with the two copies
  on an in-memory UDP network (`source/ssr_net.c`, `source/ssr_split_net.c`).
* Sound: the game has one listener. 3D sounds use the nearer player's camera,
  both players' characters speak, and player 2's racer gets the player's
  engine sound (`source/ssr_split_audio.c`).

### Names

* In earlier builds the SD folder was `sd:/switch/sonicracing/` and the NRO
  `SonicRacing.nro`. The first start moves the old folder's files into
  `sd:/switch/sonic_allstars_nx/` (`source/dcr_migrate.h`).
* The program inside the NRO keeps the name `sonicracing_nx.nsp`, so that
  installed older builds still recognise a newer NRO as an update.

## For people porting other 32-bit games

* Check the ABI first: armeabi games are usually softfp. Build the wrapper
  softfp too, and every shim can be called directly.
* Look for kernel user helper calls (literals 0xffff0fa0-0xffff0fe0) in the
  game's library.
* Expect text relocations, both in the game's library (apply them before
  mapping) and in your own program (see `crt0_reloc.c`).
* Plan for the 1 GiB heap region and a reserved region for the game module.
* Threads that spin while waiting for another thread will hang on Horizon at
  equal priority on one core. Use a priority the kernel time-slices and allow
  several cores.
* Keep enum sizes consistent across every library boundary (libnx32, Mesa,
  FFmpeg, your code).
* A 32-bit program is started as an ExeFS override; the launcher in
  `launcher/` shows one way to install it from an NRO.
* Soft-float engines can be sped up a lot by replacing libgcc's float helpers
  with VFP code in place.
* Useful for debugging: `source/exc_handler.c` writes `crash.log` with
  registers and addresses as module + offset.
* Ryujinx 1.1.1098 runs 32-bit programs with a few differences: no VCVT
  fixed-point, no `set:cal` GetSerialNumber, and the time service's shared
  memory may not map.
