# schwung-shimwash
Shimwash — shoegaze shimmer reverb for Ableton Move (Schwung)

Two independent pitch shifters (each PRE, FDBK or POST) around a big, dark 8-line reverb. Input filter, ducker, bloom swell, freeze and 13 factory programs. Module id shimmerwash, shown on the Move as Shimwash.

Pages
Page	1	2	3	4	5	6	7	8
Main	Mix	Length	Size	Color	M.Rate	M.Amt	Predly	Bloom
Shft A	Pitch	Fine	Delay	Mode	Amt	Pan		
Shft B	Pitch	Fine	Delay	Mode	Amt	Pan		
Wash	Tone	Locut	Spread	Freeze				
Input	Gain	Type	Freq	Q				
Duck	Depth	Thresh	Attack	Rel				
Preset	INIT plus 13 factory programs							
Mode — PRE: the shifter eats the reverb input and sends the pitched copy into the reverb. FDBK: it eats the reverb output and feeds it back in (classic shimmer; each pass climbs another interval). POST: it eats the reverb output and adds the pitched copy to the output only.
Pitch ±24 semitones, Fine ±100 cents, Delay 0–1000 ms (an echo on the pitched sound; in FDBK it spaces out the rising repeats).
Length 0.8–60 s. Color is tail brightness (dark → open).
Freeze holds the wash: input stops, the tail sustains. A preset recall never touches it.
Duck pulls the reverb down while you play (Depth 0 = off, the default).
Input filter only affects what goes into the reverb, never your dry sound. Type LP with Freq at the very bottom cuts the whole send.

Factory programs: Slow Bloom, Cathedral, Octave Down, Angel Choir, Fifths Tower, Dark Drift, Echo Climb, Endless Pad, Parallel Sheen, Tight Sparkle, Ducked Halo, Swell Pad, Wide Open.

Build (needs only Docker)
./scripts/build.sh

Builds inside ubuntu:22.04 (glibc 2.35, the Move's) as linux/arm64, then runs every gate: module.json is generated from the plugin itself, help.json is checked against the device's rules, the real .so is loaded the way the host loads it and put through ~130 checks (page layout, enum and state round-trips, every preset, every control actually changing the audio, stability and tails, freeze, CPU), and the glibc symbol ceiling is verified. Output: dist/shimmerwash/ and dist/shimmerwash-module.tar.gz.

Install
./scripts/deploy.sh move.local --reboot

Uploads under temporary names and renames them into place. Never scp straight over a live .so — Schwung keeps it mapped, and overwriting it in place can take the firmware down. The reboot is what loads the new build.

Alternative: Schwung Manager (http://move.local:7700) → Modules → Install Custom Module → From Tarball → dist/shimmerwash-module.tar.gz.

Requires a Schwung that provides named knob pages and the preset browser (0.12 or newer).

Layout
src/shimmerwash.c      DSP + host glue (one file)
src/module.json        GENERATED from the plugin: do not hand-edit
src/help.json          in-device help (20-char lines, ASCII)
src/host/*.h           Schwung's plugin headers, vendored
tools/                 module.json generator, help checker, contract gate, host simulator
tests/dsp_suite.c      DSP behavior report (RT60, spectrum, delay, ducker, filter)
scripts/               build.sh, docker-build.sh, deploy.sh, Dockerfile
Notes
Original implementation from standard published techniques (feedback delay network reverb, delay-line pitch shifting). Not derived from any commercial plugin. MIT licensed.
module.json and the plugin's own chain_params can never disagree: the file is produced from the built plugin, and the gate fails if it is stale.
Not included: a browser remote panel, and user-writable preset slots. The factory programs are compiled in; a module's state (every control) is saved with the Schwung set as usual.
