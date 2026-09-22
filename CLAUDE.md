# intaglio

Engraved line shading — an FFGL effect plugin for Resolume Arena/Avenue that
turns a clip into a ruled copper plate, where tone is carried by the pitch and
weight of the line. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. MIT. ID `IG01`, display name `SW Intaglio`.

Read `AGENTS.md` before changing the phase pass, the coverage arithmetic, or
anything about how the hatch sets combine.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render offline: `./build/igtest --out /tmp/frame.png`
- The test card on its own: `./build/igtest --card /tmp/card.png`
- One region of a frame, for a figure: `./build/igtest --out docs/flat-panel.png --crop 0.53,0.34,0.73,0.88`
- List parameters: `./build/igtest --list`
- Set anything by name: `./build/igtest --set "Coherence=0" --set "Sets=2"`

## Verify
- Everything: `tools/verify.sh` (~20 s: fresh universal build, every check, the
  sweep, lipo, plist, ad-hoc signature, `oxbow probe`)
- **The ruling**: `./build/igtest --pitch` — lines counted across a flat field
  against width over pitch, at two rasters, which must agree exactly.
- **The weight curve**: `./build/igtest --weight` — coverage at five tones, plus
  the cross-hatch union at one, two and three sets.
- **The two limits**: `./build/igtest --limits` — solid at black, bare at white,
  checked against the model *and* against the render.
- **The angles**: `./build/igtest --perpendicular` and `--crosshatch`.
- **The flow field**: `./build/igtest --flow` — nib's measurement, re-run
  against this repo's copy of the shader.
- **The checks can fail**: `./build/igtest --negative` — every check above run
  against a deliberately wrong model, and required to fail.
- No dead controls: `python3 tools/sweep.py`
- Render cost: `./build/igtest --bench`
- Universal + exports: `lipo -archs` and `nm -gU … | grep _plugMain` — never
  trust the build log for either.

## Notes
- **Tone is carried by the line.** Coverage is a function of the local
  darkness; the line's half-width is half the coverage times the pitch. The two
  ends are the claim: coverage 1 is a solid plate, coverage 0 is bare paper.
- **The hatch coordinate is an integral, not a projection.** The phase pass
  walks from the frame centre to each phase texel accumulating `n . dq` and
  `t . dq`. Exact for a uniform field and for concentric circles; an
  approximation elsewhere, and necessarily so. Don't replace it with
  `dot( p - centre, n( p ) )` — that shatters far from the centre, and the
  reason is in AGENTS.md.
- **One walk serves all three sets.** A set at `phi` has coordinate
  `( cos phi * A + sin phi * B ) / pitch`.
- **Every length is a fraction of the frame height**, never pixels. That is what
  makes the plugin scale-invariant and what lets every check run at two rasters
  and demand agreement.
- **Two sets at the same angle are one set.** `FoldCoincidentSets` — without it
  Cross Angle at 90° with three sets prints the same share twice in the same
  place and the plate comes out 11% short.
- **The flow is believed by `sqrt( l1 - l2 )`, not by anisotropy.** Anisotropy
  is scale free, so steering by it makes the plate follow the noise floor across
  every plain area in the picture.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it.
  Option parameters hold the element value.
- `patch`, `sample`, `input`, `output`, `filter`, `common`, `active`, `half`,
  `layout`, `flat` are GLSL reserved words, and `step`, `distance`, `mix` are
  built-ins that must not be shadowed. Shader errors surface only at runtime,
  in the diagnostics log. `tools/verify.sh` greps for the reserved ones because
  glslc is optional and skips.
- Randomness in shaders is integer hashing (PCG-style), never `fract(sin(...))`.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `intaglio_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- Local repo only: no GitHub remote, no tag, not registered on the website.

## Not done yet
- Never loaded into Resolume; no OFX port; no browser demo; no `--pipe`/
  `--script`, so the fleet's video pipeline cannot film it. No factory presets.
  Never built on Windows or Linux.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It exists for the failure that actually happens: a shader that will
not compile, which otherwise looks like "the plugin does nothing" with no
message anywhere. It records which of the seven passes it was, and the GL
vendor/renderer next to it.

    ~/Library/Logs/intaglio/intaglio.YYYY-MM-DD.log

`InitGL` calls `diag::init()` before anything else. **nib does not**, which is
why nib's log has never been written — see AGENTS.md.
