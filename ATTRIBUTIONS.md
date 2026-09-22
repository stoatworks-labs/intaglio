# Attributions

intaglio is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. intaglio is not
> registered there yet, so this copy is hand-written. Register it before release
> — and note that the script's `--only` flag truncates the file rather than
> filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for
the OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Ships with macOS. The offline harness links it to deflate its PNG output, which
is why the PNG writer in `tools/igtest/main.cpp` is fifty lines rather than a
vendored dependency. Nothing in the shipped plugin uses it.

## Work from elsewhere in the fleet

### nib — the flow field

<https://github.com/stoatworks-labs/nib>
Licence: MIT
Copyright: Stoatworks Labs

**The structure tensor pass, the tensor blur and the eigen decomposition in
`kFlowLibrary` are copied from nib**, not re-derived — the same way astable
took vectrix's beam renderer. Copying was deliberate: a second eigen
decomposition is a second thing to get wrong, and "obeys the same flow field as
nib" is a claim this plugin makes and would rather be able to keep. `igtest
--flow` is nib's own measurement, re-run against this repo's copy of the
shader rather than assumed to carry over.

Two things were changed and both are marked where they live:

- **`flowAt` returns a fourth number**, `sqrt( l1 - l2 )`, and this plugin
  steers by that rather than by nib's normalised anisotropy. nib only needs to
  know whether to *turn* a walk it is already taking; a plate has to decide
  whether to curve its ruling at all, and anisotropy is scale free — a gradient
  of one code value in a flat sky reads as directional as the edge of a
  building. See the comment over `flowAt` in `source/Shaders.cpp`.
- **The gradient is taken at a scale**, off a mip chain, because `Detail` has to
  move it here; nib moves scale with the band-pass downstream, and there is no
  band-pass downstream here.

### tinsel, outrun, rosette

<https://github.com/stoatworks-labs/tinsel>
Licence: MIT
Copyright: Stoatworks Labs

`PassBuffer` (`FFGLFBO` with the SDK's colour-texture leak fixed and three
sampling modes), `Diag`, the CMake shape, the harness shape, `tools/sweep.py`
and `tools/verify.sh` all come from these. rosette's screen-angle and
registration checks are the model for measuring a lattice, and its `--register`
comment is where the lesson about running a raster-sensitive check at more than
one raster was first written down.

## Method

The structure tensor as the right way to average orientations, and the edge
tangent flow built from it, are from the published computer-graphics
literature — Kang, Lee and Chui's flow-based difference of Gaussians and the
coherence-enhancing filtering line of work it sits in. They are described in
papers, not copied from anyone's source; the implementation here is nib's, and
nib's is this fleet's own.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
