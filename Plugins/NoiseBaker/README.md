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

`bBipolarOutput` emits [-1,1] instead of [0,1], applied last as `v * 2 - 1`. It is the only range control.

The chain is `normalize -> distribute -> polarity -> encode`, and normalization always targets the full [0,1] using the observed range, so every level is used regardless of polarity.

There is deliberately **no symmetric normalize mode**, because symmetric normalization is a property of the *source*, not the destination. It matters when a basis emits genuinely signed values, so that raw zero and output zero coincide. Every basis here returns [0,1], so raw zero is the bottom of the range rather than a meaningful centre, and normalizing symmetrically against it would strand the data in part of the range and waste the rest.

Where the zero crossing lands is set by the distribution stage instead:

| Distribution | Output zero sits at |
|---|---|
| None | Midpoint of the observed range |
| CenterMedian | The median, so equal volume above and below |
| Equalize | The median, and the field is uniformly distributed |

**CenterMedian is the right pairing for a displacement field**, since equal mass either side of zero is what gives no net drift.

When a signed basis arrives -- curl of a vector potential -- a symmetric mode returns, scoped to that basis rather than to the output polarity.

## Storage and decode

**Bipolar output requires RGBA16F.** Validation rejects the BGRA8 combination.

BGRA8 is a UNORM format: the texture unit converts `byte/255` to [0,1] in fixed-function hardware before the value reaches any shader, and filtering happens in that space too. There is no asset setting that changes this -- SNORM would be a different pixel format, and `SRGB` only inserts a gamma curve. So a signed channel has to be bias-encoded back into [0,1] on write, which means the texture viewer shows it unsigned, a raymarch samples it unsigned, and the field only reads correctly if every consumer remembers the decode. Nothing about the asset signals that, so the failure is silent and looks like a bake problem rather than a sampling one.

It also halves precision, spending 256 levels across [-1,1] for a step of about 1/127, and warp error is amplified by the gradient of whatever it displaces.

On RGBA16F the encode is identity and negatives are stored directly, so bipolar values show up negative in the viewer and in a marcher with no decode step. Half-float precision is non-uniform, denser near zero, which suits a field centred there. 8 bytes per voxel: 128^3 is 16 MB, 256^3 is 128 MB -- fine for warp and curl volumes, which want low resolution anyway.

Consumers still use one uniform rule, recorded per channel in `UNoiseBakeAssetUserData`:

    Value = Stored * DecodeScale + DecodeBias

which is `(1, 0)` for everything the baker now produces. The encode path for signed BGRA8 is retained but unreachable, so the contract still holds for textures baked before the rule existed.

Format has no effect on the octave budget. The Nyquist rule counts voxels per lattice cell, not bits per channel, so the table above is unchanged. The only indirect cost is memory: if 128 MB pushes you from 256^3 down to 128^3, that costs an octave.

## Normalization groups

Channels sharing a non-zero `NormalizationGroup` have their probe ranges merged and one scale applied across all of them. `0` means independent.

This exists for vector-valued output. Normalizing vector components independently rescales each axis differently, which rotates and skews every vector in the field — and the result still looks like plausible noise, so nothing in the preview reveals it. For a curl or warp field, put R, G, B in group 1 and leave A at 0. Members must share a normalize mode; validation enforces it. Grouping is what a signed basis will build on once curl lands.

## Vector field bakes

`UVectorFieldBakeRecipe` bakes a derivative of a scalar potential into RGB. Separate asset from the packed recipe because the parameter surface differs: one basis, period, octave count and seed driving a single potential, not four independent ones.

Curl and Gradient share the asset for the mirror-image reason -- their surfaces are *identical*, and the difference is one enum value rather than a different set of knobs. They are the two pure cases of the Helmholtz decomposition, which says any vector field splits into a curl-free part and a divergence-free part.

| Mode | Divergence | Curl | Behaviour | Cost |
|---|---|---|---|---|
| **Curl** `∇×P` | zero | nonzero | Pure swirl; stirs without clumping | 12 evals, 3 instances |
| **Gradient** `∇f` | nonzero | zero | Sources and sinks; attract/repel | 6 evals, 1 instance |

Curl is for advection and warps that move material around. Gradient is the attractor/repulsor field: `+∇f` points uphill toward maxima, so negate at the sample site for attraction. It is baked as `+∇f`, the mathematical definition, since a negate is free at the sample site.

Gradient magnitude goes to zero at maxima and minima. That is correct, not a defect -- a peak is a stable fixed point with no pull.

Three things are forced on the RGB channels rather than authored, each for its own reason. `NormalizationGroup = 1`, because independent scales rescale each axis differently and rotate every vector. `bBipolarOutput`, because sign is direction. And `DistributionMode = None` -- redistribution is monotone and so harmless on a scalar, but on a vector each component would get a *different* remap, which changes every direction. There is no per-component redistribution that preserves a vector field.

For Curl the potential is one authored recipe at three decorrelating seeds. Both extremes fail: three independently authored fields have sources and sinks everywhere, and one instance for all three components collapses the cross terms and confines the output to a plane. The seed offsets avoid multiples of `0x9E3779B9`, which is what `PN_FBM` adds per octave, so no component lands on another's octave.

### Alpha

| Mode | Carries |
|---|---|
| Independent | Its own authored scalar field |
| Vector Magnitude | `\|∇×P\|` or `\|∇f\|` |
| Scalar Potential | `f` itself; Gradient mode only |

Scalar Potential is usually the best default in Gradient mode: `f` is already evaluated so storing it is free, and one fetch then gives both the field value and its direction of steepest ascent.

Vector Magnitude looks redundant against `length(rgb)` but is not, once mips exist. Averaging vectors that point different directions shortens them; averaging magnitudes does not. So `length(mip2.rgb)` underestimates local strength while a separately mip-filtered magnitude preserves it. Worth a channel only if consumers sample lower mips.

### Gain

Differentiation multiplies each octave by its frequency, so where the potential's octave amplitudes fall as `Gain^i`, the output's fall as `(Gain × Lacunarity)^i`. At the packed default of 0.5 with lacunarity 2 that product is exactly 1.0 -- flat spectrum, fine octaves dominate, output looks like static.

Vector field recipes default to `Gain 0.3, Octaves 3`, and validation warns when the product reaches 1.0.

### Epsilon

Central-difference step in voxels, quarter voxel by default. Too small and the difference of two nearly equal FBM values is dominated by float cancellation; too large and it spans a lattice cell and smooths away the finest octave. Validation checks both bounds, the upper one against the finest octave's cell size rather than an absolute number.

Tiling survives differentiation, since a finite difference of a periodic function is periodic with the same period -- so the tiling self-test applies unchanged and is worth leaving on. Mips survive too: curl and divergence are linear operators and so is a box filter.

## Bake stamping

Every successful bake writes a fresh GUID and increments `BakeVersion`, on both the recipe and the texture's `UNoiseBakeAssetUserData`. Once anything downstream depends on the field, that stamp is how you detect the texture in front of you is not the one your content was authored against.

## Known risk points on first compile

Two things to check if the build fails:

1. **`SHADER_PARAMETER_ARRAY(FIntVector4, ...)`** — if your engine version lacks a `TShaderParameterTypeInfo` specialisation for `FIntVector4`, swap `ChannelParamsB`/`ChannelParamsC` to `FUintVector4`, or to `FVector4f` with an `(int)` cast in the shader. Seeds would then need clamping below 2²⁴.
2. **`TMGS_SimpleAverage` on a volume texture** — mip generation support for volume textures has moved around across 5.x. If mips come out wrong, set `bGenerateMips = false` and verify before shipping.

Shader iteration: `r.ShaderDevelopmentMode 1` in `ConsoleVariables.ini`, then `recompileshaders changed` after editing the `.ush`/`.usf`.

## Not in this MVP

CPU / FastNoise2 backend, pseudovolume 2D export, cross-channel mixing, curl and domain warp bases, baked gradients, in-editor slice preview widget.
