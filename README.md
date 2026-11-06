# IW7S

IW7S is a client for **Call of Duty®: Infinite Warfare** designed for **playing offline against bots**, with public matches that feel as if real players were in them.

It is based on [IW7-Mod](https://github.com/auroramod/iw7-mod) and is the result of a joint effort between [joseupp](https://github.com/joseupp) and **Claude** (Anthropic's AI): together we designed, programmed and tested every one of its improvements.

> ***You must own [Call of Duty®: Infinite Warfare](https://store.steampowered.com/app/292730/Call_of_Duty_Infinite_Warfare/) on Steam.*** The game will not start if your account does not own it. Pirated copies are not supported.

Current version: **r1000b** (beta).

---

## What's new

### Offline public matches
- **Public lobbies that work without servers**, with their countdown, team assignment and transition into the match, just like the original game.
- **Bots that behave like players:**
  - They join the lobby gradually, each with their own name, rank, prestige, specialist and appearance.
  - They keep their identity and team in the match.
  - After the match most of them stay for the next one and some leave, just like a real lobby.
- **Balanced mixed difficulty.** Each bot's rank matches its difficulty: veterans have high prestige levels and recruits have low ranks.
- **Bot menu in the lobby:** number of allies and enemies, and the difficulty of each team.
- **"Start match" button**, so you don't have to wait for the lobby to fill up.
- **"Find match" goes straight into the lobby.** The game mode is changed from the lobby options.
- **Editable match rules**, as in Black Ops 3. Your changes are kept until you pick another mode.
- **You earn XP and rank up** even offline.
- **Message of the day** in the multiplayer menu, with its image and pop-up window, and with text you can customise.

### Graphics and textures
- **Custom textures:** drop an image into the `iw7s\images` folder and it will always replace the game image with the same name. Works for weapons, camos, interface, icons, etc.
- **Dumping of the game's images**, so you can edit them.
- **`png2dds` tool**, which converts your images to the game's format.
- **PlayStation controller icons** ready to use.

### View and HUD
- **Weapon FOV independent** from the view FOV, as in modern Call of Duty games.
- **Wider and better-tuned FOV:**
  - Aim-down-sights zoom consistent with the chosen FOV.
  - Sniper scopes as in the original.
  - Properly framed winners' circle.
- **Adjustable HUD tilt.** You can make it flat, keep it as the original or choose something in between. The top part is adjusted separately and moves closer to the screen edges.
- **Ammo bar** in multiplayer.

### Convenience and stability
- **Everything in the game folder:** profiles, settings, logs and scripts. AppData is not used.
- **Steam doesn't need to be open.** You only need to open it the first time, to verify that you own the game.
- **No external console window.**
- **The game closes cleanly**, without the error that sometimes appeared on exit.
- **Playlist and settings can be edited** from the `iw7s\dw` folder.
- Private matches without a countdown (optional).

---

## Texture commands

Open the in-game console (`~` key) and type:

| Command | What it does |
|---|---|
| `images_dump` | Dumps all the images currently loaded to `iw7s\images_dump` (as PNG) |
| `images_dump <name>` | Dumps only the images whose name contains `<name>` (e.g. `images_dump camo_76`) |
| `images_dump_dds` | Same as `images_dump`, but saves the images in the game's DDS format |
| `images_dump_dds <name>` | Same as `images_dump_dds`, but only the matching images |
| `images_reload` | Reloads the `iw7s\images` folder after adding or changing images |

Only the images in use at that moment are dumped: to get a weapon or camo texture, have it on screen first (in a match or in the menu).

**How to replace a texture**
1. Dump it with `images_dump <name>`.
2. Edit the PNG and save it in `iw7s\images`, keeping the same name.
3. Optional: convert it to DDS with `png2dds` (run without arguments to convert everything in `iw7s\images`), so it loads faster and looks like the original.
4. Restart the game, or run `images_reload`.

---

## Installation

1. Copy `iw7s.exe` into the root of your Call of Duty: Infinite Warfare folder.
2. Copy the `data\iw7s` folder from this repository into the root of the game folder, so you end up with `<game folder>\iw7s`.
3. The first time, open Steam with your account and run `iw7s.exe`. After that, Steam doesn't need to be open.

## Building

- Clone the repository with [Git](https://git-scm.com/install/windows). **Do not download it as a ZIP**, as it would not include the dependencies.
- Run `generate.bat`.
- Build `build\iw7-mod.sln` with Visual Studio 2022.

## Credits

- **[joseupp](https://github.com/joseupp) and Claude (Anthropic)**: IW7S.
- [IW7-Mod](https://github.com/auroramod/iw7-mod) (AuroraMod): the client it is based on.
- [s1-mod](https://git.alterware.dev/alterware/s1-mod), [h1-mod](https://github.com/auroramod/h1-mod), [h2-mod](https://github.com/alicelys/h2-mod) and [momo5502](https://github.com/momo5502): codebase and research.

## Disclaimer

This software was created for research and personal use. It is not affiliated with Activision or Infinity Ward. Use it responsibly. Licensed under GPL-3.0 (see `LICENSE`).
