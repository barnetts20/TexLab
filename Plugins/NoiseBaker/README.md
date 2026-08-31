# Noise Baker

Bakes 4-channel packed, exactly-tiling `UVolumeTexture` assets from a recipe data asset via a GPU compute pass.

## Install

Drop the `NoiseBaker` folder into `<Project>/Plugins/`, regenerate project files, rebuild. The folder name must be `NoiseBaker` — the runtime module looks itself up by that name to map the shader directory.

## Use

1. Content Browser → Miscellaneous → Data Asset → `NoiseBakeRecipe`.
2. Set `Resolution` and `TargetPackagePath`.
3. Configure the R/G/B/A channels independently.
4. Hit **Validate Only** to check the rules without spending a bake.
5. Hit **Bake Volume Texture**.

The texture is created at `TargetPackagePath/TargetAssetName` on the first bake and reused on every subsequent one.

## Bake pipeline

| Stage | What it does |
|---|---|
| Validate | Resolution power-of-two, integer lacunarity, Nyquist headroom, brick divisibility |
| Tiling self-test | Compares `f(uvw)` to `f(uvw + 1)` on each axis at 32³ |
| Probe | Half-res pass to observe each channel's raw range, padded 2% |
| Main | Full-res dispatch in Z slabs, supersampled, normalized and shaped in-shader |
| Quantize | Float4 → BGRA8 with optional triangular dither |
| Bricks | Per-brick RGBA min/max into asset user data |
| Write | `Source.Init` → `PostEditChange` → save |

## Why lacunarity must be an integer

Exact tiling comes from wrapping the lattice cell index modulo the period inside the hash. Every FBM octave has to wrap at the same place, so every octave's period must be a whole number of cells across the tile. Octave *i* uses `BasePeriod × Lacunarity^i` cells, which is only an integer for every *i* if `Lacunarity` is an integer.

Fractional lacunarity is the usual trick for breaking up visible lattice alignment, and it is not available here. If grid structure shows through, raise `BasePeriod`, change the basis, or vary the seed per octave rather than reaching for fractional lacunarity.

Wrapping the input *position* instead of the cell index does not work: interpolation at `x = Period - 0.5` still reaches cell index `Period`, which hashes differently to cell 0. That produces a hard discontinuity rather than a seam-free wrap, and it is the single most common way this gets implemented wrong.

## Base period and octave budget

Base period is restricted to powers of two: 1, 2, 4, 8, 16, 32, 64. Every other value is dominated. The Nyquist cap is a step function whose breakpoints land on powers of two — periods 3 and 4 permit the same octave count, as do 5 through 8 — and within a bracket the largest value gives the finest base features for the same budget.

Max octaves at lacunarity 2:

| Base period | 32³ | 64³ | 128³ | 256³ | 512³ |
|---|---|---|---|---|---|
| 1 | 4 | 5 | 6 | 7 | 8 |
| 2 | 3 | 4 | 5 | 6 | 7 |
| 4 | 2 | 3 | 4 | 5 | 6 |
| 8 | 1 | 2 | 3 | 4 | 5 |
| 16 | — | 1 | 2 | 3 | 4 |
| 32 | — | — | 1 | 2 | 3 |
| 64 | — | — | — | 1 | 2 |

`—` means the base period alone violates Nyquist; no octave count validates. Higher lacunarity costs octaves quickly, since the budget is logarithmic in it: at 128³ base 4, lacunarity 3 allows 2 octaves and lacunarity 4 allows 2.

The rule is `Resolution ≥ 4 × BasePeriod × Lacunarity^(Octaves-1)`; the finest octave needs at least 4 voxels per lattice cell or it aliases into uncorrelated hash noise and destroys the mip chain. `NoiseBakeValidation::GetMaxOctaves` computes the cap, and the validation error reports it directly.

If a channel is pure high-frequency erosion, spend the budget on base period rather than octaves — one octave at base 16 carries more usable detail per byte than four octaves at base 2.

### Scaling at sample time

Sampling the baked texture at an integer UV multiple is also tiling-safe, but scaling by *k* gives *k*× finer features **and** *k*× shorter repeat. It is a trade, not a substitute for baking a higher period.

Where it does pay is multi-lookup composition: two lookups at **co-prime** scales, say 3 and 5, have a combined period of 15 tiles. Power-of-two scales collapse back to the largest and buy nothing.

## Sampling convention

Voxel centres: `uvw = (voxel + 0.5) / resolution`. Voxel 0 and voxel N−1 sit half a voxel inside the tile on either side, so they are neighbours under wrap addressing.

Sampling at `voxel / (resolution - 1)` instead duplicates the boundary row and produces a visible seam even when the noise function itself is perfectly periodic. Anything that samples this texture must use **Wrap** addressing on all three axes; set it on the sampler in the material.

## Notes on the output asset

- Uncompressed BGRA8 (`TC_VectorDisplacementmap`). BC on volume noise moves the isosurface and shows block structure across slices.
- Mips are safe: a 2×2×2 box reduction never reads outside the volume, so every mip stays exactly periodic.
- 256³ is 64 MB, 512³ is 512 MB. Size accordingly.

## Preview material

`Shaders/Private/NoiseVolumePreview.ush` is a per-channel raymarcher for looking at a baked volume and validating tiling visually. See `Docs/PreviewMaterial.md` for the Custom node pin list and wiring.

Short version: box-bounded march, four independent Beer-Lambert integrations sharing one set of steps, `TileCount` to repeat the volume so interior seams become visible, and `DomainOffset` on Time to scroll them through view.

The bake's own self-test is the authoritative tiling check. The preview is for what the self-test cannot see — aliasing, a channel that normalized to nothing, a density scale that reads as fog.

## Distribution modes

Normalization fixes the *range* and says nothing about the *distribution* within it, and the archetypes differ enormously. Perlin FBM is near-Gaussian and clusters tightly. Worley F1 is heavily skewed low, because volume grows as r³ so most space is far from any feature point. Ridged variants pile up at the top. So `pow(v, 2)` nudges Perlin slightly dark and nearly annihilates Worley F1 — that's the per-noise tweaking redistribution removes.

Every mode is a **monotone remap of value**, so it preserves every level set exactly: `{v > t}` becomes `{v > t'}`. Features don't move, isosurfaces keep their geometry. Distribution changes, shape doesn't.

| Mode | Does | Cost |
|---|---|---|
| **None** | Nothing | — |
| **Center Median** | `v^g` with `g = ln(0.5)/ln(median)`, endpoints pinned | Fixes midpoint, not spread |
| **Equalize** | Applies the CDF; output uniform on [0,1] | Flattens each basis's character |

Equalize is the one that makes exponents fully portable, at the cost of the clustering that makes Perlin read as soft. That's recoverable, but **not with an exponent** — `pow` has no inflection, so it shifts mass up or down but can't gather it toward the middle. That needs an S-curve: `smoothstep(0, 1, v)` re-clusters, and applied twice approaches Gaussian. The advantage of restoring character that way is that the same S-curve then means the same thing on every channel.

Measured by a second probe pass at the Normalized stage, after ranges are settled. Adds ~1% to bake time. The applied mode, gamma and observed median are recorded in `UNoiseBakeAssetUserData`, since redistribution is baked in and not detectable from the texture afterwards.

## What gets baked, and what doesn't

**Anything applied inside the octave loop must be baked. Anything applied to the finished value can be deferred to the consumer.**

Ridged folds each octave before summing, so it cannot be reconstructed from the finished FBM — it's baked. Domain warp and curl will be the same. But an output range remap and an inversion are both post-sum scalar maps, so a consumer reproduces either with one multiply-add at the sample site. Neither exists here.

Range narrowing isn't just redundant, it's lossy: baking `[0, 0.5]` into BGRA8 spends 128 of 256 levels achieving what a multiply does for free. Channels always occupy the full unit interval.

Distribution mode is the exception that proves the rule. It acts on the finished value, but Equalize needs the CDF of the entire volume, and a consumer sampling one texel doesn't have it.

## Bipolar output

`bBipolarOutput` emits [-1,1] instead of [0,1], applied last as `v * 2 - 1`. It's the only range control, and it earns its place by changing the **decode contract** rather than the value: on BGRA8 the stored bytes are identical to the unipolar case, and what differs is the recorded `DecodeScale`/`DecodeBias`. On RGBA16F negatives are stored directly.

Consumers use one rule either way: `Value = Stored * DecodeScale + DecodeBias`.

A bipolar channel must use **Auto (Symmetric About Zero)**; validation rejects plain Auto, which would put the zero crossing wherever the probe landed.

Asymmetric signed ranges, floors, and headroom all belong at the sample site — `v * (max - min) + min` is one instruction and costs no precision.

## Signed output

Output ranges may go negative. Two things follow.

**Normalize mode.** A signed channel must use **Auto (Symmetric About Zero)**, which normalizes with one magnitude `M = max(|min|, |max|)` about zero. Plain Auto maps the observed minimum to 0, putting the zero crossing wherever the probe happened to land — a constant offset, which for a displacement field means a net drift. Validation rejects the combination.

**Storage.** `BGRA8` bias-encodes signed channels into the unsigned texture, spending 256 levels across `[-1,1]` for a step of about 1/127. Fine for density, marginal for warp: warp error is amplified by the gradient of whatever it displaces, so a stair-step in the offset becomes a stair-step in the result. `RGBA16F` stores signed values directly at 8 bytes per voxel — 64³ is 2 MB, 128³ is 16 MB, which is nothing for a warp volume.

Consumers decode uniformly with `Value = Stored * DecodeScale + DecodeBias`, both recorded per channel in `UNoiseBakeAssetUserData`. That's `(1, 0)` for unsigned and for all RGBA16F, `(2, -1)` for signed BGRA8.

`bInvert` reflects about the range midpoint rather than computing `1 - v`. Identical for `[0,1]`, but on `[-1,1]` the old form would have flipped the sign *and* shifted by 1.

## Normalization groups

Channels sharing a non-zero `NormalizationGroup` have their probe ranges merged and one scale applied across all of them. `0` means independent.

This exists for vector-valued output. Normalizing vector components independently rescales each axis differently, which rotates and skews every vector in the field — and the result still looks like plausible noise, so nothing in the preview reveals it. For a curl or warp field, put R, G, B in group 1 and leave A at 0. Members must share a normalize mode; validation enforces it.

## Toward curl

Signed storage and shared normalization are the storage-side prerequisites, and they're in. Real curl noise still needs one more piece: the three components must come from the **curl of a single vector potential**, not from three independently-seeded scalars. Three unrelated scalars in RGB give a field with sources and sinks everywhere, which is exactly what curl noise exists to eliminate.

The remaining work is a vector-valued channel group that evaluates a periodic potential and takes its curl by central differences at bake time, with epsilon around a quarter voxel — small enough to resolve the finest octave, large enough to avoid float cancellation. Tiling survives it, since the curl of a periodic field is periodic, and so do mips, since divergence is linear and so is a box filter.

## Bake stamping

Every successful bake writes a fresh GUID and increments `BakeVersion`, on both the recipe and the texture's `UNoiseBakeAssetUserData`. Once anything downstream depends on the field, that stamp is how you detect the texture in front of you is not the one your content was authored against.

## Known risk points on first compile

Two things to check if the build fails:

1. **`SHADER_PARAMETER_ARRAY(FIntVector4, ...)`** — if your engine version lacks a `TShaderParameterTypeInfo` specialisation for `FIntVector4`, swap `ChannelParamsB`/`ChannelParamsC` to `FUintVector4`, or to `FVector4f` with an `(int)` cast in the shader. Seeds would then need clamping below 2²⁴.
2. **`TMGS_SimpleAverage` on a volume texture** — mip generation support for volume textures has moved around across 5.x. If mips come out wrong, set `bGenerateMips = false` and verify before shipping.

Shader iteration: `r.ShaderDevelopmentMode 1` in `ConsoleVariables.ini`, then `recompileshaders changed` after editing the `.ush`/`.usf`.

## Not in this MVP

CPU / FastNoise2 backend, pseudovolume 2D export, cross-channel mixing, curl and domain warp bases, baked gradients, in-editor slice preview widget.
