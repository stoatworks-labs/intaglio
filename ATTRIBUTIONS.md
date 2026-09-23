# Attributions

Intaglio is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Flow field: structure tensor, tensor blur and eigen decomposition — Stoatworks nib

<https://github.com/stoatworks-labs/nib>  
Licence: MIT  
Copyright: Stoatworks Labs

The structure tensor pass, the tensor blur and the eigen decomposition in the flow library are copied from nib, not re-derived. A second eigen decomposition would be a second thing to get wrong, and 'obeys the same flow field as nib' is a claim this plugin makes. nib's own flow measurement is re-run against this repo's copy. Two changes, both marked in the source: flowAt also returns sqrt(l1 - l2), and the ruling is steered by that rather than by nib's scale-free anisotropy; and the gradient is taken at a scale, off a mip chain, so that Detail can move it.

### PassBuffer, diagnostics, build and harness shape — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

Same fleet, copied rather than shared. PassBuffer (FFGLFBO with the SDK's colour-texture leak fixed and three sampling modes), the Diag log, the CMake shape, the offline harness shape, tools/sweep.py and tools/verify.sh come from tinsel, outrun and rosette. rosette's screen-angle and registration checks are the model for measuring a lattice, and the practice of running a raster-sensitive check at more than one raster.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Flow-based difference of Gaussians and coherence-enhancing filtering

Using the structure tensor to average orientations, and the edge tangent flow built from it, come from the published computer-graphics literature: Kang, Lee and Chui's flow-based difference of Gaussians and the coherence-enhancing filtering work around it. They are described in papers, not copied from anyone's source. The implementation is nib's, and nib's is this fleet's own.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
