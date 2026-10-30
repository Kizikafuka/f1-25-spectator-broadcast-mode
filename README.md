# Spectator/Broadcast Mode

**Version 0.0.32 (experimental)** · by Kizikafuka · id `kizi.spectator-hud` · F1 25 (Steam), Swofty Launcher 1.6 or newer

> ### Notice: AI Drive + Spectate is being split
> The old addon **AI Drive + Spectate** (`kizikafuka.ai-drive`) will be **removed** and replaced by **two separate addons**:
> 1. **Spectator/Broadcast Mode** (this addon) — the first one, available now.
> 2. **AI Drive** — only the "AI drives your car" part, coming next.
>
> Until AI Drive is out, the old addon stays up but gets no new features. Please don't run its spectate feature together with this addon, because both control the game's spectator camera and HUD.

## What it does

Watch the race from the game's broadcast camera while your own car is out of the picture. The race keeps going, and you can switch between drivers with the game's own keys.

### After a crash — "Crash" options
- **Keep the race running.** When your car is destroyed, the game normally ends your race. This addon stops that, so the race goes on and you spectate the rest. The camera moves to a car that is still racing.
- **Send my wreck to the garage.** After 3, 6 or 10 seconds the wreck goes to its garage and stays there, so it doesn't block the track. A restart puts you back on the grid as normal.

### Garage start — "Garage start" options
- **Park me before the race.** Your car goes to the garage while the setup and strategy screens are up, and you spectate the whole race from the start. Good for a spectator team, or for watching a Grand Prix as a broadcast.
- **Park my teammate too.** Your teammate's car goes to the garage as well. Your teammate is found from the game's UDP telemetry (the participants list), so it works on any grid.
- Needs the game's **UDP telemetry on**: Settings > Telemetry > UDP On, IP 127.0.0.1, port 20777, format 2026. If another program already listens on that port (Terminal Damage, SimHub), set its port to 20778 and pick 20778 in "Telemetry: pass a copy to another addon".

### Watching — "Watch" and "Keys" options
- **Start by itself.** Spectating starts when your car is out, and stops when you race again.
- **Move to a car still racing.** If the camera is on your wreck or on a car that retires, it switches to another car.
- **Spectate key** (default F4) turns it on and off by hand. After you turn it off it stays off until you press it again or the race restarts.
- **Camera key after spectating** is pressed once when spectating stops, so your camera goes back to normal. Set it to your game's change-camera key.

### Other options
- **Window: keep the game running.** The game doesn't pause or mute when you click another window.
- **Look: hide the game logo.** Hides the F1 logo in the corner of the spectator HUD (applies the next time you press Play).
- **Debug: detailed log.** Extra lines for bug reports.

## Good to know
- The spectator HUD stays on while you switch cars with the game's keys. The addon changes a small part of the game's code while you spectate, and puts it back when you stop.
- Your teammate and the garage cars stay parked until the race restarts or ends.
- This is experimental. If something goes wrong, send the log zip from the launcher (Send Logs).
- Made for the Steam version of F1 25 (build 25251167 or newer) and Swofty Launcher 1.6 or newer.

## Credits
- The idea of keeping your car and the race going after terminal damage comes from [Terminal Damage](https://github.com/nyannoying1337/f1-25-terminal-damage) by nyannoying. This addon has its own code, doesn't need Terminal Damage, and isn't made by or endorsed by nyannoying. Thank you for the idea and for being open about it!
- Thanks to Swofty for the official game bindings used here (garage call, spectator camera and HUD hooks).

## Build it yourself

You need [Zig](https://ziglang.org/download/) (0.15 or newer) on your PATH and the Swofty native SDK headers.

1. Copy the `native/include` folder from Swofty's native addon template (see <https://f1.swofty.net/reference/addons/template>) into `native/include` here. The headers are Swofty's, so they are not part of this repository.
2. Run `powershell -ExecutionPolicy Bypass -File build.ps1`.
3. The addon is written to `dist/kizi.spectator-hud-<version>.swoftyaddon`. Import it in the Swofty Launcher.

## Licence

MIT, see `LICENSE`. The Swofty SDK headers keep their own licence.
