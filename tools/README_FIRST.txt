Sonic & SEGA All-Stars Racing for Nintendo Switch (32-bit wrapper)
==================================================================

This runs the Android game Sonic & SEGA All-Stars Racing (2013) on a Switch
with Atmosphere. It contains no game code and no game data: you supply your
own copy of the game.

1. Copy switch/sonic_allstars_nx/sonic_allstars_nx.nro to the same place on
   your SD card.
2. Copy your own copy of the game (com.sega.ssasr 1.0.1) there too, with any
   file names (they are told apart by what is in them):
       the APK
       its expansion file, main.20.com.sega.ssasr.obb (on Android:
         Android/obb/com.sega.ssasr/), or the .zip it came in -- nothing
         needs unpacking
3. In sphaira: Homebrew > Sonic & SEGA All-Stars Racing > Install Forwarder.
4. Launch the new icon on the HOME menu. The first start installs the game
   program for that icon, restarts, and unpacks the game's library from the
   APK (once). The game's data is read where it is.

Coming from an earlier version (the folder sd:/switch/sonicracing)? The
first start moves your files there -- the APK, the data, config.ini, your
saves -- into sd:/switch/sonic_allstars_nx. The old SonicRacing.nro stays
where it is and can be deleted.

Controls
  The phone game was touch and tilt; here it plays like a console game.
  The touchscreen still works the menus, as on a phone.

  In a race (Mario Kart 8 Deluxe's layout)
    A                    accelerate
    B                    brake / reverse
    R or ZR              drift (hold while steering); let go in the air: trick
    L or ZL              item / All-Star move (a thrown item: hold to aim,
                         let go; push the stick or D-pad back to throw it
                         behind you)
    X                    look behind
    Left stick / D-pad   steer (config.ini: or the controller's motion,
                         turned like a wheel)
    +                    pause
    A                    skips the fly-by before the start, and continues
                         from the results
  The stick steers by how far you push it (fine near the centre; config.ini
  stick_curve), eased like a steering wheel. Throttle and brake are on/off:
  the Switch's buttons and triggers are digital, and so is the game's own
  accelerator.

  In the menus (the button prompts at the bottom right say what does what)
    D-pad / left stick   move between the buttons (a soft glow shows where
                         you are); on the card carousels, turn them
    L / R                turn a carousel, page a list
    A                    press
    B                    back (on the pause menu: resume)
    X / Y                the extra buttons: the rules (track select), the
                         mission info, the racer's stats
    +                    resume (pause menu), start (title screen)
    Right stick          a free pointer: A touches (hold to drag); the left
                         stick or the D-pad brings the frame back
  Your licence name is typed on the Switch keyboard.

  The HUD is the console one: the item box at the top left, the racers'
  progress bar in the middle, LAP and POS at the right, no touch buttons.

Two players (split screen)
  SPLIT SCREEN on the main menu, as on the consoles: the Switch's
  controller screen for two players comes up first (one Joy-Con each, held
  sideways, works). Then GRAND PRIX, SINGLE RACE, BATTLE or VS RACE
  (player 1 chooses). Both players' racer selects are side by side, player
  1 on the left, player 2 on the right, each on their own controller: A is
  READY (B takes it back), and the game goes on once both are. The first
  time, player 2 picks their licence (or makes a new one) in their column.
  Player 1 then picks the course or the cup. The course's flyover and the
  racers' introductions are on the whole screen; from the countdown the
  screen splits, player 1 on top, with the AI racers, the voices, items and
  all: each player's item box and POS / LAP at their outer corners, one
  progress bar across the middle (P1 and P2 under the players' portraits).
  Either player's + opens the game's pause menu for both; player 1 uses it.
  Both players are heard: each one's engine, items and character's voice,
  what happens near either of them as loud as it is to them, the music
  once. When one player is over the line, the other has 30 seconds to finish
  (the countdown in their half); out of time, or last of everyone still
  racing, they are placed where they are and the race ends.
  BATTLE and VS RACE are the game's own LOCAL battle and race (the phone's:
  the two copies of the game play them together, nobody hosts or joins):
  VS RACE is the two players alone, no AI racers. Both pick a racer, player
  1 picks the arena or the course (and the laps, the items) and starts (+);
  each player's half is their own, with both halves' sound (each sound
  once, from the nearer player). B on the racer select leaves for the mode
  select.
  A second copy of the game runs for player 2's licence, racer select, HUD,
  battles and VS races (its saves in data/p2, a copy of yours at first).
  config.ini [multiplayer] split_screen = false puts back the phone's
  LOCAL / ONLINE menus.

  The main menu's SPLIT SCREEN card is the Xbox 360 edition's, built in.
  The BATTLE and VS RACE cards' pictures are the phone's own LOCAL battle
  and race cards, read from your game data.

Everything unlocked
  All racers, tracks, Grand Prix cups and missions are open (config.ini
  [game] unlock_all). Your save is not changed: set it to false and you
  have exactly what you unlocked yourself.

Graphics: 60 fps, 1080p docked / 720p handheld, anisotropic filtering, the
engine's batched renderer. The game's engine was built for phones to emulate
floating-point maths in software; the port runs it on the Switch's FPU and
sets the CPU to 1785 MHz (config.ini: cpu_clock) so races hold 60 fps.
Overclocking tools (sys-clk and the like) come first: once one sets a
clock, the game leaves it alone. cpu_clock = system never touches it.

Settings: sd:/switch/sonic_allstars_nx/config.ini, written on the first start with
every option explained (steering by stick or motion, auto-accelerate, the
stick's curve, 720p / 1080p, 30 / 60 fps, the button prompts, the CPU clock,
volumes, language, the intro, everything unlocked, split screen). Saves are
kept in sd:/switch/sonic_allstars_nx/data/files/.

Online services (Google Play Games, SEGA ID, the store, ads) are gone: the
game plays offline, everything it has unlocked by playing.

If something goes wrong: sd:/switch/sonic_allstars_nx/debug.log (and
debug.prev.log, the run before) and crash.log (config.ini [debug]
log_touches = true adds every press to debug.log). To
update, copy a newer sonic_allstars_nx.nro over the old one. To remove the game
program from the icon, delete
sd:/atmosphere/contents/<the icon's title id>/exefs.nsp.
