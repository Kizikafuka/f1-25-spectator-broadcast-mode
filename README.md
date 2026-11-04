# Spectator/Broadcast Mode

**Beta, no promises.** For F1 25 on Steam (build 25251167 or newer) with Swofty Launcher 1.6 or newer. Tested in Grand Prix races only.

> **Please read:** this is the first of two addons that replace **AI Drive + Spectate**. The AI part is now the separate addon **AI Drive**. Don't use the old AI Drive + Spectate together with this addon: both control the same camera.

## What is it?
When you crash out, the game normally ends your race. This addon keeps the race running and lets you **watch the rest of it** with the game's own broadcast camera, switching between drivers with the game's keys. You can also start a race with your car (and your teammate) already in the garage and just watch.

## How to use it
- **After a crash:** just keep the default settings. Your race goes on, the camera moves to a car that is still racing, and your wreck is moved into the garage after 6 seconds.
- **By hand:** press **F4** to start or stop watching at any time. If you switch it off, it stays off across restarts, even while AI Drive drives, until you press F4 again or start a new session.
- **Watch the whole race from the start:** turn on **"Garage start: park my car before the race"**. Your car goes to the garage while you are on the setup screens, and you watch from the start. Turn on **"park my teammate too"** for the second car (needs UDP, see below).
- **While AI Drive is driving:** with **"Watch: start by itself"** on, it switches to the broadcast camera by itself. Turn that off if you only want the AI to drive.

## Settings
- **Watch:** start by itself; move to a car that is still racing (races only: in practice and qualifying the camera stays where you put it, and the safety car is never picked); **automatic camera director** (new, see below).
- **Keys:** the watch key (F4) and your own camera key, pressed once when watching stops so your camera comes back.
- **After a crash:** keep the race going; put my wreck in the garage after 3, 6 or 10 seconds.
- **Garage start:** park my car before the race; park my teammate too.
- **Window:** keep the game running when you click another window.
- **Look:** hide the game logo (applies the next time you press Play).
- **Advanced** (in the launcher's Addons page): telemetry port, pass a copy to another program, detailed log.

## Automatic camera director (new in 0.0.39, not tested in the game yet)
**Watch: automatic camera director: Broadcast (TV style)** switches the camera by itself while you watch, like a TV director. It only picks who to show; it never moves the camera itself. It works in **races only** and does nothing before the lights go out.
- **Crashes and retirements** (and a car stopped on the track): it cuts to the car after about 2 seconds, even in the middle of another shot.
- **Overtakes:** shows the car that made the pass for about 10 seconds. A pass is only shown if the current shot is about to end.
- **Pit stops:** the stops of the top six cars cut away from a fight. The other cars' stops only show when nothing else is going on.
- **Battles:** a car within about 0.7 seconds of the one in front. It stays while the fight lasts and drops it when nothing happens or the cars drift apart.
- **Quiet moments:** the leaders, in turn, 25 seconds each.
- **Start:** the leaders for the first 30 seconds. **Last lap:** the leader when it starts and for the last 25 seconds to the flag. **Chequered flag:** the winner, then the fights still going on for position.
- Every shot is held for at least 20 seconds (crashes, the start, the last lap and the finish are the exceptions). Retired cars are never shown, apart from the moment they retire.
- **If you change the camera yourself,** the director waits 10 seconds. It does nothing while the game is paused.
- It follows the TV-director recipe used in the F1 Manager 2024 mod. The hold times and gaps are a first guess and will probably need tuning for F1 25.

## Turn on UDP telemetry (for "park my teammate too" and the camera director)
Settings > Telemetry: UDP **On**, IP **127.0.0.1**, port **20777**, format **2026**, rate **20 Hz** if the game offers it. The addon only opens that port while one of these two options is on. If another tool needs the same port, set its port to 20778 and pick 20778 under "Advanced: pass a copy".

## Do not use it together with Manual Pit Lane
Manual Pit Lane writes your car's controller while it is in the pit lane. Together with the garage start (your car is sent to the garage) that fights the game and can crash it, so the launcher treats the two as conflicting: turn one of them off.

## Recommended settings (what we tested)
- **Defaults are what we tested:** keep the race going ON, wreck to the garage after 6 seconds, start by itself ON, move to a car that is still racing ON, watch key F4, keep the game running ON.
- **Your own camera key:** set it to the key you use to change camera in the game (default C), so your camera comes back when watching stops.
- **Garage start:** leave it OFF unless you want to watch a whole race without driving. "Park my car" works on its own. "Park my teammate too" needs UDP telemetry and is **not confirmed in the game yet**.
- **Launcher:** tested on Swofty Launcher 1.6.x. **1.7.0 is not tested yet.** If something breaks on 1.7.0, please send the log zip.
- **Tested:** Grand Prix races: a crash keeps the race going, the camera moves to a car that is still racing, the wreck goes to the garage, the HUD stays when you switch cars, and it works together with AI Drive.
- **Not tested yet:** practice, qualifying, sprint races, red flags, career and My Team, "park my teammate", a restart after a garage start, and the whole automatic camera director (only its rules are tested, on made-up data).

## Known limitations
- Tested in Grand Prix races only. Practice and qualifying are not tested.
- Without UDP telemetry only your own car is parked, not your teammate.
- Parked cars stay in the garage until the race restarts.
- Because it changes a small part of the game's code while you watch, a game update can break it. The log then says `NOT found` for the missing part.
- If something goes wrong, send the log zip from the launcher (Send Logs).

## For the curious (technical)
- **Keep the race going:** drops the game's "car destroyed" message for your car (a wrap on `Ne_Post_BusMessage`), only for your car and only that message.
- **Garage:** the game's pit lane function (official binding `Game_Pitlane_SetVehicleStatus`, status 9 = in the garage).
- **HUD:** the game hides the HUD every frame while the camera follows another car. While you watch, 20 instructions in the game's code (the "hide" reason) are changed so the HUD stays, and they are restored when you stop.
- **Teammate:** found from the team ids in the game's UDP participants packet.
- **Camera director:** reads four UDP packets of format 2026 on the same port: session (race or not, laps, paused), lap data (position, lap, gap to the car in front, pit status, result status), car telemetry (speed) and events (overtake, collision, retirement, lights out, chequered flag). The rules live in `tv_director.hpp` and are tested offline with `tools/tvd_test.cpp`. The camera is moved with Swofty's `spectator::follow`, the same call the "move to a car that is still racing" option uses; the packet layouts are the standard ones and are checked in the log (lines `DIRECTOR session packet: ...`).

**Credits:** the idea of keeping your car after terminal damage comes from [Terminal Damage](https://github.com/nyannoying1337/f1-25-terminal-damage) by nyannoying. This addon has its own code, doesn't need Terminal Damage and isn't endorsed by nyannoying. Made by Kizi and RK; the spectate code comes from AI Drive + Spectate. Thanks to Swofty for the official game bindings.

Source: https://github.com/Kizikafuka/f1-25-spectator-broadcast-mode (MIT).

**Changelog 0.0.38:** first release. Your car is now also found before AI Drive takes it, so "keep the race going" works while the AI drives. A debug trace was removed.
