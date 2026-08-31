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

## The Nyquist rule

The finest octave lays `BasePeriod × Lacunarity^(Octaves-1)` cells across the volume. Validation requires at least 4 voxels per cell there. Below that the top octave aliases into uncorrelated hash noise, and it will not survive mip generation either.

At 128³ with lacunarity 2 and base period 4, that caps you at 6 octaves. Raise resolution or lower base period to buy more.

## Sampling convention

Voxel centres: `uvw = (voxel + 0.5) / resolution`. Voxel 0 and voxel N−1 sit half a voxel inside the tile on either side, so they are neighbours under wrap addressing.

Sampling at `voxel / (resolution - 1)` instead duplicates the boundary row and produces a visible seam even when the noise function itself is perfectly periodic. Anything that samples this texture must use **Wrap** addressing on all three axes; set it on the sampler in the material.

## Notes on the output asset

- Uncompressed BGRA8 (`TC_VectorDisplacementmap`). BC on volume noise moves the isosurface and shows block structure across slices.
- Mips are safe: a 2×2×2 box reduction never reads outside the volume, so every mip stays exactly periodic.
- 256³ is 64 MB, 512³ is 512 MB. Size accordingly.

## Bake stamping

Every successful bake writes a fresh GUID and increments `BakeVersion`, on both the recipe and the texture's `UNoiseBakeAssetUserData`. Once anything downstream depends on the field, that stamp is how you detect the texture in front of you is not the one your content was authored against.

## Known risk points on first compile

Two things to check if the build fails:

1. **`SHADER_PARAMETER_ARRAY(FIntVector4, ...)`** — if your engine version lacks a `TShaderParameterTypeInfo` specialisation for `FIntVector4`, swap `ChannelParamsB`/`ChannelParamsC` to `FUintVector4`, or to `FVector4f` with an `(int)` cast in the shader. Seeds would then need clamping below 2²⁴.
2. **`TMGS_SimpleAverage` on a volume texture** — mip generation support for volume textures has moved around across 5.x. If mips come out wrong, set `bGenerateMips = false` and verify before shipping.

Shader iteration: `r.ShaderDevelopmentMode 1` in `ConsoleVariables.ini`, then `recompileshaders changed` after editing the `.ush`/`.usf`.

## Not in this MVP

CPU / FastNoise2 backend, pseudovolume 2D export, cross-channel mixing, curl and domain warp bases, baked gradients, slice preview UI.
