# intaglio — orientation for another LLM (or a newcomer)

**What it is:** engraved line shading as an FFGL 2.1 effect plugin for Resolume
Arena/Avenue. It turns a clip into a ruled copper plate: tone is carried by the
pitch and weight of the line, not by a grey. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT. Public at
`github.com/stoatworks-labs/intaglio` since 2026-09-23, **released at v0.1.0 on
2026-09-23**, registered on the website and in stoatworks-backend; never loaded
into Resolume on macOS. User guide: `docs/USER-GUIDE.md` (the only copy anyone
edits), rendered to `docs/USER-GUIDE.pdf` and to
https://stoatworks-labs.com/software/intaglio/guide/ by the website's
`build_guides.py`.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the phase pass, the coverage arithmetic, or the
way the hatch sets combine.

Family notes: the copy pass, `PassBuffer`, `Diag`, the CMake shape, the harness
shape, `sweep.py` and `verify.sh` are lifted from **tinsel**, **outrun** and
**nib**; the lattice measurements owe their method to **rosette**. Where those
repos document a trap, it applies here too.

---

## The one idea

**An engraver cannot choose a grey.**

The plate has lines. Tone is made by how close together they are and how fat
each one is: a line *swells* where the tone darkens and tapers where it
lightens. At the dark limit the line is as wide as the pitch and the plate goes
solid; at the light limit it tapers to nothing and leaves bare paper. Those two
limits are the whole claim, and `igtest --limits` checks both against the model
as well as against the render.

Everything else falls out rather than having been arranged: the shading of an
engraving, detail surviving in the darks as line weight rather than as mud, and
the moiré where two hatch sets cross.

### This is not nib, and the difference is the point

**nib draws the edges. Intaglio draws the tone** — lines everywhere, whose
spacing and weight carry the shading, which happen to *obey* the edges because
they follow the same flow field.

The test that separates them is a flat grey area: nib draws nothing there,
because there is no edge, and intaglio has to fill it with evenly spaced lines
of the pitch and weight that grey calls for. The harness's card carries such a
panel on purpose, and `docs/flat-panel.png` is a crop of it.

So the flow field is **copied from nib, not derived a second time** — the same
way astable took vectrix's beam renderer. A second eigen decomposition is a
second thing to get wrong, and "obeys the same flow field as nib" is a claim
this plugin would rather be able to keep. `igtest --flow` is nib's own
measurement re-run against *this* repo's copy of the shader text, not assumed
to carry over. What was changed is in ATTRIBUTIONS.md and marked in the source.

**`burin` is not related.** It is also named after the engraver's tool — the
steel rod cut to a lozenge point — but it is an SVG vector renderer that draws
artwork at the resolution the frame needs. It shares nothing with this but a
dictionary entry. Nobody needs to go and check.

---

## The hatch coordinate, which is the whole of the new work

A hatch line is a curve that runs along the structure. To draw a *family* of
them, evenly spaced, you need a scalar `h` whose level sets are those curves
and whose gradient is one over the pitch; the lines are then `h = 0, 1, 2, …`
and the distance to the nearest one is `|h - round( h )| * pitch`.

**Such an `h` cannot be written down per pixel.** The obvious attempt,
`h = ( p - c ) . n( p )` with `n` the local normal to the flow, looks right and
is not. Its gradient is `n + ( dn/dp )' ( p - c )`, and the second term scales
with the distance from the origin: at a thousand pixels out, a flow that turns
through one degree over its smoothing length moves the phase by most of a
pitch between adjacent pixels. The lines shatter. Measuring from the frame
centre halves the distance and fixes nothing.

So the phase is **integrated**, in its own pass:

    A( p ) = integral of n . dq   along the straight ray from the centre to p
    B( p ) = integral of t . dq   along the same ray

- For a **uniform** field this is exactly `( p - c ) . n` — a perfect grating,
  which is why `--pitch`, `--weight` and `--limits` are exact on a flat field.
- For a field of **concentric circles** it is exactly the radius, wherever the
  circles are centred, because along any path `n . dq` is `d|r|`. The rings on
  the test card come out as rings.
- **One walk serves every hatch set.** A set at `phi` to the first has normal
  `cos phi * n + sin phi * t`, so its coordinate is
  `( cos phi * A + sin phi * B ) / pitch`. No second integral, and the whole of
  `--crosshatch` is about that one line of arithmetic.

### What it does not do, and cannot

**An evenly spaced family of curves following an arbitrary direction field does
not exist.** `h` with `grad h = n / pitch` requires the normal field to be
curl-free, and a general one is not. This is not an implementation shortcoming:
it is why real engravings *bifurcate*, and why a burin line splits into two
where the spacing would otherwise open up.

The integral's particular failure is a **radial shear**. Two neighbouring
pixels' rays are nearly the same path, so their phases agree; but a ray that
passes through a strong feature accumulates phase its neighbour did not, and
the lines bunch and shear along the ray that grazes the feature. On the
harness's card — which is deliberately hostile, with a ring field of sixteen
pixels' period and a hard-edged panel through the frame centre — you can see
it as a seam running out from the middle.

Two things keep it usable, and both are controls:

- **Flow Smoothing.** A smoother field turns less along the ray, so less phase
  is accumulated that the neighbour did not.
- **Coherence.** The belief is `clamp( sqrt( l1 - l2 ) * gain )`, so the plate
  only curves its ruling where there is real structure. That single change was
  the biggest visual improvement in the build — see the traps.

A better construction exists and is a v0.2 idea: solve `laplacian h = div( n / pitch )`
by multigrid instead of integrating along a ray. That is a genuine global
least-squares fit to the same wish, it has no preferred origin and no radial
seam, and it costs about a dozen ping-ponged passes.

### Why the phase buffer is sized from the ruling

The phase is smooth — it is an integral — so it does not need the picture's
resolution, it needs the *ruling's*. `PhaseDivisor` returns the largest power
of two at or below a third of a pitch, so there are always three or four phase
texels per pitch whatever the raster, and the phase pass costs about the same
at 720p and at 4K: a 59-line plate gets a 320×180 buffer at 720p and a 480×270
one at 4K. `--bench` prints the buffer it used next to the time.

`kMaxPhaseSide` caps it, and that cap is a real limit rather than a tidy one:
at the very finest rulings on a 4K frame the phase is sampled below the
ruling's own Nyquist rate, and the finest lines alias.

---

## The traps

Ordered by how much time they cost.

**An orientation estimator built on the picture's gradient is biased where two
hatch sets cross, and the bias grows as the raster shrinks.** `--crosshatch`
was first written with an orientation histogram of the coverage's gradient. It
read **61.07°** at 960×540 and **59.84°** at 432×243 for sets that are 62°
apart by construction. The cause is that the first set's lines *chop* the
second's into segments, and the gradient at a chopped end is a mixture of the
two normals — so the two peaks are pulled toward each other, and more so when
there are fewer pixels across a line. A single-raster check would have shipped
with a 2° tolerance and a 1° bias inside it. The fix is `gratingResponse`: the
discrete Fourier transform of the coverage, evaluated at exactly the frequency
the model predicts, under a Hann window. Chopping is a *multiplication*, and a
product's spectrum is the second set's line convolved with the first set's comb
— replicas at other frequencies, not a shift of the original. It now reads
**62.00°** at both rasters.

**Cross Angle at 90° with three sets put the third set on top of the first.**
Set `k` runs at `k * Cross Angle`, so at 90° set 3 is at 180°, which is set 1:
same direction, same phase, same lines. Each was being asked for its own share
of the tone, and printing that share twice in the same place left the plate
**eleven per cent short** of the tone it was asked for. `FoldCoincidentSets`
merges the engagements of sets that rule at the same angle before the shares
are worked out, in both the shader and the C++ mirror. Only 0° and 90° are
degenerate, so it does nothing the rest of the time.

**Three sets at exactly 60° are phase-locked, and the union formula is then an
approximation.** The three wave vectors satisfy `k2 = k1 + k3` exactly, so the
triple overlap is not the product of the three coverages and the plate
over-covers by about **2.5%**. Two sets at any angle, and three at any
non-resonant angle, are exact to a thousandth. This is not fixed; it is
measured, reported by `--weight`, and bounded there by a tolerance that is
labelled as measured rather than derived.

**Anisotropy is scale free, and steering by it makes the plate follow the noise
floor.** `( l1 - l2 ) / ( l1 + l2 )` is 1 for a gradient of one code value in a
flat sky just as much as for the edge of a building, so a plate steered by it
curves its ruling to follow whatever the noise says across every plain area of
the picture — which is what the first build looked like, and it looked like a
fingerprint. nib is not wrong to use it: nib only needs to know whether to
*turn* a walk it is already taking. A plate has to decide whether to curve at
all. `flowAt` therefore returns a fourth number, `sqrt( l1 - l2 )` — the same
judgement with the magnitude left in, a tone gradient per pixel — and
Coherence is a gain on that.

**A linear 8-bit ramp makes its own staircase, and the staircase is not scale
free.** `--perpendicular` was first written against a ramp across the whole
frame. That is at most 255 code values, so it arrives as steps several pixels
apart; the steps are perpendicular to the gradient and bias nothing, but their
*ends* land on the pixel grid and the jaggies they leave are axis-aligned. The
bias was 0.32° at 960×540 and **1.54°** at 432×243 — again growing as the
raster shrank, because the step spacing is fixed in code values while
everything else in this plugin scales with the frame. A plane sinusoid whose
wavelength is a fraction of the frame height carries several code values per
pixel at any size. It now reads 0.01° and 0.03°.

**nib never calls `diag::init()`.** The logger is copied from it, and in nib
nothing sets `g_ready`, so every `diag::info` and `diag::error` is dropped on
the floor and `~/Library/Logs/nib/` has never existed — including the one
message the logger exists for, which is *which of the ten shaders would not
compile*. tinsel, rosette and graticule all call it; nib is the odd one out.
`IntaglioPlugin::InitGL` calls it first. (nib's `Diag.cpp` also reads
`TINSEL_LOG_DIR` as its override variable, which is the same copy-paste going
the other way.)

**`distance` and `step` are GLSL built-ins**, and `patch`, `sample`, `input`,
`output`, `filter`, `common`, `active`, `half`, `layout` and `flat` are
reserved words. `patch` is a natural name in a plugin about ruled lines — it is
reserved for tessellation — and the failure is silent until runtime, because
these shaders are assembled from strings and the "syntax error, line N" points
into a file that does not exist. `tools/verify.sh` greps for all of them as
declared identifiers, because glslc is optional and a machine without it skips
the whole shader step.

**At full weight, `<` leaves a line of bare paper down the middle of a solid
plate.** The ink test is `distPx <= w`, and at coverage 1 the half-width `w` is
exactly half the pitch, so the pixels exactly halfway between two lines are on
the boundary. With `<` they print paper, and a plate that is meant to be solid
comes back with a faint grid on it. The antialiasing width is also clamped to
`min( w, pitch/2 - w )` so the ramp stays symmetric about `w`, which is both
what keeps the coverage exactly `2w/pitch` at any Taper and what closes the
plate at the top of the range.

**`ScopedFBOBinding` does not restore the viewport** (SDK `b1afaf9`). The host
viewport is captured at the top of `ProcessOpenGL` and restored before the
composite; without it the composite inherits the phase pass's viewport, which
is a *quarter the size*, and in most viewers that reads as "blown out to white
with a small picture in the corner" rather than as a viewport bug.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** `FFGLFBO::Initialise` sizes its colour texture under one of those,
so allocating a buffer silently unbinds the input texture. Everything is
allocated in `ensureBuffers()` before anything binds. The symptom is the
dangerous part: correct on every frame except the one that allocates — and the
phase buffer reallocates whenever Line Pitch changes, which an operator drags.

**`ffglex::FFGLFBO::Release()` leaks the colour texture** — it tests
`depthBufferID` a second time where it plainly meant `colorTextureID`.
`PassBuffer::Destroy()` deletes it first.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* `SetParamRange` could widen it,
so every host parameter is 0..1 and every conversion lives in `Controls.cpp`.
Option and boolean parameters are the exception.

**`SetParamGroup` collapses runs of consecutive same-group ids.** The id order
in `Controls.h` is load-bearing: reorder it, or insert a parameter mid-enum,
and a group silently splits in two (and every saved composition renumbers).
Append only.

**The plugin registers itself from a file-scope constructor.** `intaglio_core`
is an OBJECT library and the `CFFGLPluginInfo` lives in `PluginEntry.cpp`,
listed only in the MODULE target. A STATIC core drops the registration TU and
ships a bundle that loads, exports `plugMain`, and contains no plugins.

**The FFGL name field is `char[ 16 ]` and is not null-terminated**, so a host
truncates a long name without saying so. `SW Intaglio` is eleven characters.
`oxbow probe` is the only thing in the loop that reads it back the way a host
does, and `verify.sh` runs it.

---

## Shape of the code

    source/Intaglio.{h,cpp}  the plugin class: parameters, buffers, the chain.
    source/Shaders.{h,cpp}   all GLSL. The flow library is one string, pasted
                             into the phase pass and into the harness's probe.
    source/Controls.{h,cpp}  0..1 host parameters to physical units, and the
                             coverage arithmetic, mirrored from the shader.
    source/PassBuffer.*      FFGLFBO with tinsel's leak fix, three samplings.
    source/PluginEntry.cpp   the registration. See traps.
    source/Diag.*            a log file, for the shader that will not compile.
    tools/igtest/            the offline harness (drives the real classes).
    tools/sweep.py           no control is silently dead.
    tools/verify.sh          all of it.

Pass chain: copy → tone (mipmapped) → tensor → tensorBlur×2 → **phase** →
engrave → composite. Only the phase pass runs at a reduced size.

### What is mirrored and what is not

**The filtering is GPU-only.** There is no C++ copy of the tensor
decomposition, the tensor blur or the phase integral — a mirrored per-pixel
filter is a second implementation bought to restate the shader, and it would be
tested against itself (orrery's precedent, followed across the fleet).

What *is* mirrored, in `Controls.cpp` and marked `//= mirrored` on both sides,
is the **coverage arithmetic**: the weight curve, the engagement smoothstep,
the coincident-set fold and the cross-hatch union. That is arithmetic rather
than filtering, and it is the thing `--limits` and `--weight` make an exact
claim about — so it is worth being able to state the claim about the model and
then check the render against it.

And the mirror is not tested against itself. Every number `--weight` and
`--limits` print is the **GPU's** answer compared against the C++ one: ten
tones across two rasters, six union cases, twelve limit cases. A mirror that
had drifted from the shader would show up there as a disagreement, not as two
files quietly agreeing.

---

## Every numeric check, and where its tolerance comes from

This section exists because a tolerance taken from the machine it was measured
on is not a tolerance. Each row says what the number is, what it is derived
from, whether the measurement is raster-sensitive, and what it would do on a
different rasteriser at a different raster.

**The general answer on rasterisers.** Almost nothing here is rasterised in the
usual sense: the plate's geometry is computed analytically inside the fragment
shader from the interpolated `uv`, and fragment centres are fixed by the GL
specification at pixel centres. So the quantities below are *deterministic
geometry*, and what a different driver can change is confined to: the precision
of `pow`, `exp2`, `smoothstep` and `sqrt` (bounded by GLSL 4.10 §8.2); the
weights used for bilinear filtering of the phase texture, which the spec allows
an implementation to approximate; and whether 32-bit float textures are
filterable at all. The last of those is the one real portability risk and it is
listed under "assumed" below.

**On a second rasteriser (2026-09-23).** `ci.yml` runs `--flow`, `--pitch`, `--weight`, `--limits`, `--perpendicular`, `--crosshatch` and `--negative` and
`tools/sweep.py` on GitHub's macOS runner, which has no GPU, so the harness
falls back to Apple's software renderer. All of them passed there with the
tolerances below unchanged. The numbers in the table are still this machine's.

| Check | Number | Where it comes from | Raster-sensitive? |
| --- | --- | --- | --- |
| `--flow` improvement | ≥ 33% | nib's floor, restated: below a third the tensor pass is not worth its cost and the honest thing is to delete it. Not a tuned number. | Yes, and run at 960×540 and 480×270. The *absolute* error legitimately differs — the rings are a fixed fraction of the frame, so at half the height they are half as many pixels across and fixed-amplitude noise is a larger disturbance — so both rasters are held to the same **floor on the improvement**, not to the same number of degrees. Measured 93% and 87%. |
| `--flow` sample count | ≥ 1000 px | A guard, not a tolerance: it fails if the ring field has moved off the card and the measurement is averaging nothing. | Asserted at each raster (21982 and 5498). |
| `--pitch` line count | ± 1 line | **From the lattice.** Lines sit at `x = centre + k·pitch`; a window of width W contains either `floor( W / pitch )` or that plus one, depending where the centre falls. No looser number is available and none is needed. | Yes. Run at 1280×720 and 480×270, and because the ruling is a fraction of the frame height the count across the width is a property of the aspect ratio and the ruling alone — so the two must come back **identical**, not merely both within tolerance. Both 33. |
| `--pitch` preconditions | pitch ≥ 6 px; coverage in 0.25…0.67 | Asserted, not assumed. A run count needs runs: at 6 px and 40% coverage the ink is 2.4 px and the paper 3.6 px, both wide enough to survive a half-coverage threshold at the *smaller* raster. | The precondition is checked at each raster separately. |
| `--weight` coverage | 0.005 | Three terms. (a) **Lattice sampling**: the model is continuous and the render samples it at pixel centres; the ink profile is C1 (a smoothstep), so its Fourier coefficients fall off as the cube of the harmonic, and a unit pixel grid can only fold a harmonic onto the mean near a resonance — holding the pitch ≥ 12 px and the angle off every multiple of 45° keeps the lowest foldable harmonic near `n = pitch`, which is order 1e-3. (b) **The tone buffer is R16F**: eleven bits of mantissa is 2⁻¹² in the darkness, and the curve's slope is at most `gamma·( Max − Min ) ≤ 4`, so ≤ 1e-3. (c) **`pow`**: ≤ 1.7e-6 by §8.2, negligible. The input's own 8-bit quantisation is *not* in the budget, because every expectation is computed from the quantised code value. Measured worst 0.0005. | Yes. Run at 1024×576 and 448×252, whose pitches in pixels differ by a factor of 2.3 and therefore have different resonances. |
| `--weight` raster agreement | 0.003 | Half the absolute tolerance, and it is also **the measurement of term (a)**: the two rasters differ only in the lattice sampling. Measured 0.0000. | It *is* the raster check. |
| `--weight` union, 1 and 2 sets, and 3 non-resonant | 0.005 | Same budget. Two gratings at an angle cover exactly `c1·c2` between them — integrate along one set's lines and the other's phase advances linearly, so it equidistributes — so the independence formula is not an approximation here. Measured ≤ 0.0009. | Single raster (1024×576); the quantity is an area fraction, and the raster term is already bounded by the row above. |
| `--weight` union, 3 sets at 60° | 0.04 | **Measured, not derived, and labelled as such in the source.** The three wave vectors satisfy `k2 = k1 + k3`, so the sets are phase-locked and the triple overlap depends on the absolute phases. It is a regression guard, not a claim. Measured 0.025. | Not raster-sensitive in any way that matters: it is a resonance of the model, not of the sampling. |
| `--limits` model | 4e-6 | **From the GLSL specification, not from this GPU.** Coverage at a black input is `pow( 1, gamma )`; §8.2 gives `log2` an absolute error under 2⁻²¹ on [0.5, 2] and `exp2` three ULP, so `pow( 1, gamma )` may return `1 ± ( gamma·2⁻²¹·ln2 + 3·2⁻²³ )`, which is 1.7e-6 at gamma 4. Doubled for margin. This machine returns exactly 1.0; a conforming driver need not, and a tolerance taken from this machine would have shipped a check that fails on somebody else's. | Not raster-sensitive: it is arithmetic. |
| `--limits` render | 1e-3 | The bound above plus the sliver of paper it opens: at `target = 1 − 1.7e-6` the half-width falls short of half a pitch by the same relative amount, so the bare fraction is of the same order — about 2e-5 all told. 1e-3 is fifty times that, and still four hundred times inside the failure it catches, which is a plate that does not close up and leaves a *visible* lattice. Measured 1.000000 and 0.000000. | Yes, run at 960×540 and 400×225, × three set counts, × both ends. At the limits there is no lattice left to sample, so the two agree exactly — which is the point of running it. |
| `--perpendicular` | 2° | The estimator is a Fourier response on a quarter-degree grid with a parabolic refinement, so its own resolution is under a tenth of a degree. What the tolerance allows for is the **flow field**, which `--flow` measures at about a degree on a noisy ring field; this card is clean and its orientation is constant, so a degree is generous and two is twice that. Fifteen times inside the 30° failure the check is built to catch. Measured 0.00° and 0.02°, agreeing to 0.02°. | Yes, run at 960×540 and 432×243, and required to agree to 1°. This is the check whose first version failed that agreement (1.85° against a 2° allowance) and led to the 8-bit-ramp trap above. |
| `--crosshatch` angle | 1° | **The estimator's number, because the model's is zero**: on a flat field there is no structure, so the sets rule at exactly Angle Offset and Angle Offset + Cross Angle. Quarter-degree grid plus parabolic refinement is well under a tenth; the only thing that could shift a peak is the beat between the sets, which lands at 1.03× the pitch frequency at 51° for the angles used and is excluded by the 20° guard. Measured 0.00° at both rasters. | Yes, run at 960×540 and 432×243, and required to agree. |
| `--crosshatch` single-set ratio | < 0.20 | A shape judgement rather than a measurement, and the gap is what makes it safe: one set's runner-up grating carries **0.002** of the peak and two sets' carries **1.000**, so the threshold sits a hundredfold from one side and fivefold from the other. | Measured at both rasters. |
| `--negative` | must fail | Not a tolerance: five deliberately wrong models — a ruling 15% finer, the flow turned off, a flat weight curve, the sets 18° further apart, 99% weight instead of 100% — each required to make its check report a failure. A check that cannot fail is not a check, and the way a numeric check stops being able to fail is that a tolerance grows past the error it was meant to catch. All five detected. | Runs whatever the underlying check runs, so at two rasters where that check does. |
| `sweep.py` | exact equality | No tolerance at all: two renders of the same size with one parameter moved must differ in at least one byte of the PNG. | Runs at 480×270 only; a dead uniform is dead at any size. |
| `--bench` | none | Reported, never asserted. A time is a property of the machine. | Reported at 720p, 1080p and 4K, with the phase buffer it used. |

### What that pass changed

Three things, and two of them were real defects rather than loose numbers:

1. `--crosshatch`'s estimator, which was biased and **whose bias grew as the
   raster shrank** — caught only because the check ran at two rasters.
2. `--perpendicular`'s test card, for the same reason and with the same shape.
3. `--weight`'s tolerance, which started at 0.008 and came down to 0.005 once
   the dominant term had an argument behind it rather than a measurement.

And one thing it deliberately left alone: the 60° union tolerance, which is
measured and says so.

---

## Decisions taken without asking

- **No factory presets.** The spec's control list has none and the checks do
  not need them. Adding the preset machinery would have meant importing the
  host-echo problem tinsel solved for the fleet, for a table nobody has written
  yet.
- **No temporal filter, and `SetTimeSupported( false )`.** There is no
  Stability control in the spec and the plate is a pure function of the frame,
  so the plugin holds no history at all — no ping-pong buffers, and nothing for
  a synthetic clock in the harness to drive. Declaring time unsupported stops a
  host calling `SetTime` sixty times a second into a function that discards it,
  and it is what `oxbow probe` prints. nib and the rest of the fleet leave the
  SDK's default in place; they animate and this does not.
- **Invert inverts the driving tone**, not the two colours. It makes a
  white-line engraving — the burin cutting the highlights — which is a real
  idiom and a genuinely different drawing, where swapping Ink and Paper is the
  same drawing photographed.
- **Line Pitch is reversed**: turning it up widens the spacing and gives fewer
  lines, because the control is named for the pitch and not for the count.
- **The third set's threshold is Engage + a fixed 0.22**, rather than its own
  control. A separate one would be a parameter nobody moves, and the gap is
  what makes three sets read as a progression rather than as two that arrive at
  once.
- **Lengths are fractions of the frame height, not pixels.** A plate has a
  ruling and the plate is the picture, not the monitor.
- `StoatworksAbout.h` is **generated** by stoatworks-backend's `sync-about.py`
  and `ATTRIBUTIONS.md` by `sync-attributions.py`; the project is registered in
  the website's `projects.json` (beta, with a guide) and in the backend's
  sync-about TARGETS, names.json, visibility.json and derived.json. The guide
  link made the About block five entries — About, User guide, Project page,
  Source on GitHub, Support the work — so `Controls.h` gained
  `PT_ABOUT_BUTTON_4` on 2026-09-23 (the `static_assert` against
  `about::kParamCount` caught it), and the plugin has 30 parameters in all
  (`igtest --list`): 25 controls plus About. The sweep's 25 is unchanged.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (Apple Silicon, macOS 26.4.1):**
everything in the table above, plus a universal bundle that exports `plugMain`,
a plist whose version agrees with `CMakeLists.txt` and with
`StoatworksAbout.h`, a bundle that ad-hoc signs and verifies, and `oxbow probe`
reading `SW Intaglio` / `IG01` / effect out of it the way a host does, with
all five control groups plus About in the order `Controls.h` declares them.

**Verified on GitHub (2026-09-23):** every check in the table above except
`--bench`, and the control sweep, passed on the GPU-less macOS runner (Apple's
software renderer); the Windows x64 DLL compiled with MSVC in `release.yml`.

**Windows, in Resolume Arena 7.27.1** (win-lab, llvmpipe, 2026-09-23): a CI build passed the fleet's Arena gate 9 of 9 — loads, registers as `SW Intaglio` / `IG01` / effect, all 31 host parameters as declared, renders, and all 26 probed controls move the picture.

**Assumed, or not done:**

- **Never loaded into Resolume on macOS.** How the parameters present — whether the two
  colour triples show as swatches, whether five groups read sensibly in the
  inspector — is untested.
- **32-bit float textures are assumed to be linearly filterable.** The phase
  buffer is `GL_RG32F` read with `GL_LINEAR`, which desktop GL has supported
  since 3.0, but the specification lets an implementation approximate the
  filter weights and nothing here would notice if a driver fell back to nearest
  — the symptom would be lines that stair-step every few pixels. Nothing checks
  it directly. The checks have now passed on a second driver (Apple's software
  renderer, in CI), which is evidence that it filters well enough for them, not
  a test of the filter.
- **Not built on Linux, and never run on an Intel Mac.** Windows is built:
  `release.yml` compiles the x64 DLL with MSVC. The universal build's x86_64
  slice has never executed.
- **No OpenFX port.** The browser demo exists — see *The browser demo* below.
  `igtest --pipe`/`--script` exist
  (added 2026-09-23, harness only, rztest's format) and are what the fleet's
  project video was rendered through; they are not a check and assert nothing.
- **The `Bite` and `Burr` models are judged by eye.** Both are zero-mean or
  additive decorations on top of a coverage that is measured, and both are set
  to zero by every measurement check, so nothing here says they are right —
  only that they are alive and do not disturb the thing that is measured.
- **The 4K figure is 4.57 ms**, 28% of a 60 fps frame. The phase pass is
  roughly constant with resolution, so most of that is the full-resolution
  tensor and blur. It is the number to watch if the chain grows.

---

## The browser demo

`demo/` is the page at **intaglio-demo.stoatworks-labs.com**, built on the shared
kit in `infrastructure/stoatworks-backend/resolume-demo/` (vendored into
`demo/vendor/` by its `sync.sh` — fix a kit bug there, never here). Added
2026-09-24.

**What is the plugin's own code: all of the picture.** Intaglio is seven GPU
passes and no CPU stage worth the name, so the page runs every one of them —
copy, tone and its mip chain, structure tensor, the separable tensor blur, the
phase walk, the engraving and the composite — in the plugin's order and at the
plugin's formats (RGBA16F, R16F, and the RG32F phase buffer). The nine shader
strings are copied into `demo/plugin.js` unedited, and the phase pass is
assembled the way `PhaseShaderSource()` assembles it. `demo/tools/check_shaders.py`
compares all nine character for character and checks the assembly order on both
sides; `tools/verify.sh` runs it.

**What is a port, checked by a reader and nothing else.** Every
`...FromParam` in `Controls.cpp`, and `PitchPixels`, `PhaseDivisor`, `tapsFor`,
`pow2AtMost` and `pow2AtLeast` from `Intaglio.cpp`, with the uniforms
`ProcessOpenGL()` sets. They are done in JavaScript doubles where the plugin
uses floats. Checked once by hand against the figures in this file: a 59-line
plate sizes a 320×180 phase buffer at 720p and 480×270 at 4K, as the plugin
does.

**What the page leaves out.** The About block (a text line and four buttons that
open a browser). There is no audio caveat, because the plugin has no audio path,
and nothing temporal, because it declares `SetTimeSupported( false )`.

**Decided without asking.** The clip list starts on the synthetic scene and then
the ramps — the flat panels are what separate this plugin from nib. The presets
are the page's own (the plugin ships none) and are plain parameter values. A
line under the canvas reports the ruling and the phase buffer the page sized
from it, because that sizing is the plugin's least visible decision.

**Left stale on purpose.** `docs/USER-GUIDE.md` still says there is no browser
demo. The guide is rendered to a PDF and to the website by `build_guides.py`,
which is a release chore rather than a demo one; correct it at the next guide
sync.

Deploy with `cf-run npx wrangler deploy` from the repo root; there is no build
step. Verify by content, not by status code:
`curl -s 'https://intaglio-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.
