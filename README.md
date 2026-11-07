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

## Web panel (new in 0.1.0, beta)
**Web panel** (off by default) is a web page on your own PC with the live race, like a timing screen. Turn it on in the addon settings (**On**, or **On, open my browser when it starts**), then open `http://127.0.0.1:8765/` in a browser. The address is also copied to the clipboard each time the panel starts. It needs the game's UDP telemetry on (see below); without it the page says what is missing.
- **Click a driver's name** (in the table, the track strip, the battles list or the race feed) and the spectator camera follows that car. Spectating must already be on (press your spectate key first). Buttons switch between TV pod and Broadcast; the arrow keys go to the previous/next car. A click pauses the automatic camera director for 60 seconds (the page shows "Auto director paused, click to resume").
- **Timing table:** position, gap to the leader, interval, tyre, last and best lap, PIT / OUT / STOPPED tags, and an arrow when a car is closing in or falling back.
- **Track strip:** every car as a dot along the lap, with yellow-flag zones highlighted. **Yellow flags**, the safety car and the virtual safety car show in a banner on top.
- **Battles:** cars within 1.5 seconds of the car in front, with how fast they close and how many laps until they could pass ("in overtake range" under 1 second).
- **Race feed:** passes, contact, retirements, fastest lap, safety car changes, penalties, pit in/out, a car slow or stopped on the track (a possible spin), yellow flags appearing and clearing. Click an entry to watch that car.
- **Telemetry of the watched car:** speed, gear, rpm, throttle, brake, steering, tyre temperatures and wear, fuel laps.
- **Safety:** this PC only (127.0.0.1), a secret token for the click commands, no CORS. It opens no extra UDP port: it shares the one the camera director uses. When Swofty's own telemetry API is available the panel could use it instead (not yet).
- **Not checked on every packet layout yet:** the yellow zones, the event feed, tyre temperatures and fuel laps use the standard F1 UDP layouts. If something looks wrong, send the log lines `PANEL sample`, `PANEL cars` and `PANEL click`.

## Qualifying director (0.1.10, not tested in the game yet)
The automatic camera director (same setting) now also works in **qualifying and the sprint shootout**; practice and time trial stay idle. It needs the game's UDP telemetry on, like in the race.
- **Flying lap:** a car that starts a flying lap is shown (the better qualifying position first), for up to 25 seconds.
- **Purple:** a car that sets the fastest sector 1, 2 or 3, or the fastest lap, so far is shown for 9 seconds (valid laps only; the first time set is just the baseline). It takes the camera from a flying car after 6 seconds.
- **Finish line:** a flying car in sector 3 past 80% of the lap is **held until it has crossed the line**, plus 3 seconds. Only a crash cuts it.
- **Next car:** then the flying car closest to the line, the one behind it, and so on. Crashes and retirements are shown like in the race.
- A manual camera change (keys or the web panel) pauses it for 60 seconds, as in the race.

## Performance (0.1.8)
A perf report of 0.1.6 showed this addon using about 10% of a CPU core (ticks of 11-60 ms). The cause was the game call that reads a car's controller flags (about 2.5 ms each), asked many times per tick. 0.1.8 reads the flags of your car about once a second and of the other cars one at a time (a car's flags can be a few seconds old), and the web panel page is built on its own thread, not the game's. The log line `PERF last 60 s` now also shows the web panel hook, the number and cost of the car flag reads, and the page builds.

## Automatic camera director (new in 0.0.39, not tested in the game yet)
**Watch: automatic camera director: Broadcast (TV style)** switches the camera by itself while you watch, like a TV director. It only picks who to show; it never moves the camera itself. It works in **races only** and does nothing before the lights go out.
- **Crashes and retirements** (and a car stopped on the track): it cuts to the car after about 2 seconds, even in the middle of another shot.
- **Overtakes:** shows the car that made the pass for about 10 seconds. A pass is only shown if the current shot is about to end.
- **Pit stops:** the stops of the top six cars cut away from a fight. The other cars' stops only show when nothing else is going on.
- **Battles:** a car within about 0.7 seconds of the one in front. It stays while the fight lasts and drops it when nothing happens or the cars drift apart.
- **Quiet moments:** the leaders, in turn, 25 seconds each.
- **Start:** the leaders for the first 30 seconds. **Last lap:** the leader when it starts and for the last 25 seconds to the flag. **Chequered flag:** the winner, then the fights still going on for position.
- **0.0.49:** when the safety car comes in, the spectator HUD is refreshed once (spectating off and on, the same car followed again) because the yellow SAFETY CAR banner stayed on screen.
- **Presets (0.0.48):** *Broadcast* (the rules above), *Broadcast, top 10 only* (passes, fights, pit stops and quiet shots only for the first 10; crashes at any position) and *Every action* (every car on the lead lap, shots of about 12 seconds, fights from 1.0 s, the closest gap when it is quiet). Lapped cars are never shown in a fight or a pass.
- **0.0.48 changes:** a pass or fight 3-4 places nearer the front cuts in after 5-8 seconds instead of waiting 20 (no more staying with the back of the field while the top has a pass); a pass stays shown-able for 14 seconds; a fight shot ends when the gap is over 1.2 s for 2.5 seconds (was 1.5 s for 4 s).
- Every shot is held for at least 20 seconds (crashes, the start, the last lap and the finish are the exceptions). Retired cars are never shown, apart from the moment they retire.
- **If you change the camera yourself** (the game's keys, or a click in the web panel), the director waits **60 seconds**; every further change starts the 60 seconds again. In the web panel you can click the "paused" label to let it carry on at once. It does nothing while the game is paused.
- **Holding (0.1.9):** a fight or a pass that is on screen is held until it is over. Another fight starting elsewhere does not cut it; only a fight 5 places nearer the front (and only when the current pair is not about to swap places), or a pass or a leader's pit stop after about 25 seconds. Crashes, the start, the last lap and the finish always cut in.
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
