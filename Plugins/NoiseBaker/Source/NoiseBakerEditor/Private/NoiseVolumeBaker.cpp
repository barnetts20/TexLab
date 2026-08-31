#include "NoiseVolumeBaker.h"

#include "NoiseBakeRecipe.h"
#include "NoiseBakeShader.h"

#include "Engine/VolumeTexture.h"
#include "GlobalShader.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogNoiseVolumeBaker, Log, All);

#define LOCTEXT_NAMESPACE "NoiseVolumeBaker"

namespace NoiseBakeInternal
{
	/** Mirrors PN_FLAG_* in PeriodicNoise.ush. */
	static constexpr int32 FlagRidged = 1 << 0;
	static constexpr int32 FlagInvert = 1 << 1;

	/** Readback budget per slab. Keeps peak host and device allocation bounded
	 *  regardless of volume resolution. */
	static constexpr int64 SlabByteBudget = 64ll * 1024 * 1024;

	/** Upper bound on voxels evaluated in a single dispatch.
	 *
	 *  This is the TDR guard, and it is the reason the bake is chunked at all.
	 *  Windows resets the graphics driver if a single command takes longer than
	 *  the TDR delay (2 seconds by default), and a full-resolution multi-octave
	 *  Worley bake will exceed that comfortably. Splitting into slabs with a
	 *  flush between each keeps every individual dispatch well inside the
	 *  window. Scaled down by the supersample count, since each voxel costs
	 *  Supersample^3 evaluations. */
	static constexpr int64 MaxVoxelsPerDispatch = 2ll * 1024 * 1024;

	/** Fraction of the observed range added as headroom on each side after a
	 *  probe pass, since a reduced-resolution probe can miss the true extremes. */
	static constexpr float ProbePadding = 0.02f;

	static int32 ComputeSlabDepth(int32 Resolution, int32 Supersample)
	{
		const int64 VoxelsPerSlice = (int64)Resolution * Resolution;
		const int64 BytesPerSlice = VoxelsPerSlice * sizeof(FVector4f);

		const int64 SS3 = FMath::Max((int64)Supersample * Supersample * Supersample, 1ll);

		const int64 ByteLimited = FMath::Max(SlabByteBudget / FMath::Max(BytesPerSlice, 1ll), 1ll);
		const int64 CostLimited = FMath::Max((MaxVoxelsPerDispatch / SS3) / FMath::Max(VoxelsPerSlice, 1ll), 1ll);

		const int64 Depth = FMath::Min3(ByteLimited, CostLimited, (int64)Resolution);
		return (int32)FMath::Max(Depth, 1ll);
	}

	/** Deterministic per-voxel hash, used for dither. */
	static FORCEINLINE uint32 VoxelHash(int32 X, int32 Y, int32 Z, int32 Channel)
	{
		uint32 H = (uint32)X * 73856093u ^ (uint32)Y * 19349663u ^ (uint32)Z * 83492791u ^ (uint32)Channel * 2654435761u;
		H ^= H >> 16;
		H *= 0x7FEB352Du;
		H ^= H >> 15;
		H *= 0x846CA68Bu;
		H ^= H >> 16;
		return H;
	}

	/** Triangular PDF dither in roughly [-0.5, 0.5].
	 *
	 *  Triangular rather than uniform because uniform dither leaves the
	 *  quantization error correlated with the signal, which still reads as
	 *  banding once a raymarcher integrates through many samples. */
	static FORCEINLINE float TriangularDither(int32 X, int32 Y, int32 Z, int32 Channel)
	{
		const uint32 H = VoxelHash(X, Y, Z, Channel);
		const float A = (float)(H & 0xFFFFu) * (1.0f / 65535.0f);
		const float B = (float)((H >> 16) & 0xFFFFu) * (1.0f / 65535.0f);
		return (A - B) * 0.5f;
	}
}

// ---------------------------------------------------------------------------
// Parameter packing
// ---------------------------------------------------------------------------

void FNoiseVolumeBaker::BuildDispatchParams(const UNoiseBakeRecipe& Recipe, FNoiseBakeDispatchParams& OutParams)
{
	OutParams.Resolution = Recipe.Resolution;
	OutParams.Supersample = FMath::Max(Recipe.Supersample, 1);
	OutParams.bApplyNormalize = true;
	OutParams.DomainOffset = FVector3f::ZeroVector;

	TArray<FNoiseChannelRecipe> Channels;
	Recipe.GetChannels(Channels);

	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FNoiseChannelRecipe& C = Channels[Index];

		OutParams.ChannelParamsA[Index] = FVector4f(C.Gain, C.OutputMin, C.OutputMax, C.WorleyJitter);

		OutParams.ChannelParamsB[Index] = FIntVector4(
			(int32)C.Basis,
			FMath::Max(C.BasePeriod, 1),
			FMath::Max(C.Octaves, 1),
			FMath::Max(C.Lacunarity, 2));

		int32 Flags = 0;
		Flags |= C.bRidged ? NoiseBakeInternal::FlagRidged : 0;
		Flags |= C.bInvert ? NoiseBakeInternal::FlagInvert : 0;

		OutParams.ChannelParamsC[Index] = FIntVector4(C.Seed, Flags, 0, 0);

		// Identity until the probe pass (or the manual range) fills it in.
		OutParams.ChannelNorm[Index] = FVector4f(1.0f, 0.0f, 0.0f, 0.0f);
	}
}

// ---------------------------------------------------------------------------
// GPU dispatch
// ---------------------------------------------------------------------------

bool FNoiseVolumeBaker::DispatchSlab(
	const FNoiseBakeDispatchParams& Params,
	int32 SliceOffset,
	int32 SliceCount,
	TArray<FVector4f>& OutSlab,
	FString& OutError)
{
	check(IsInGameThread());

	const int32 Resolution = Params.Resolution;
	const int64 NumElements = (int64)Resolution * Resolution * SliceCount;
	const int64 NumBytes = NumElements * sizeof(FVector4f);

	if (NumElements <= 0)
	{
		OutError = TEXT("DispatchSlab called with an empty slab.");
		return false;
	}

	// Heap-allocated so the render command can hold it safely; the game thread
	// blocks on a flush before reading, but a shared pointer keeps the lifetime
	// obvious rather than relying on that.
	TSharedPtr<TArray<FVector4f>, ESPMode::ThreadSafe> Destination =
		MakeShared<TArray<FVector4f>, ESPMode::ThreadSafe>();
	Destination->SetNumUninitialized((int32)NumElements);

	TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> Readback =
		MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("NoiseBake.Readback"));

	ENQUEUE_RENDER_COMMAND(NoiseBakeDispatch)(
		[Params, SliceOffset, SliceCount, NumElements, NumBytes, Readback](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGBufferRef OutBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), (uint32)NumElements),
				TEXT("NoiseBake.Volume"));

			TShaderMapRef<FNoiseBakeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			FNoiseBakeCS::FParameters* PassParams = GraphBuilder.AllocParameters<FNoiseBakeCS::FParameters>();
			PassParams->VolumeResolution = FIntVector(Params.Resolution, Params.Resolution, Params.Resolution);
			PassParams->SliceOffset = SliceOffset;
			PassParams->SliceCount = SliceCount;
			PassParams->Supersample = Params.Supersample;
			PassParams->bApplyNormalize = Params.bApplyNormalize ? 1 : 0;
			PassParams->DomainOffset = Params.DomainOffset;

			for (int32 Index = 0; Index < 4; ++Index)
			{
				PassParams->ChannelParamsA[Index] = Params.ChannelParamsA[Index];
				PassParams->ChannelParamsB[Index] = Params.ChannelParamsB[Index];
				PassParams->ChannelParamsC[Index] = Params.ChannelParamsC[Index];
				PassParams->ChannelNorm[Index] = Params.ChannelNorm[Index];
			}

			PassParams->OutVolume = GraphBuilder.CreateUAV(OutBuffer);

			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
				FIntVector(Params.Resolution, Params.Resolution, SliceCount),
				FIntVector(FNoiseBakeCS::ThreadGroupSize));

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("NoiseBake(%dx%dx%d slice %d)", Params.Resolution, Params.Resolution, SliceCount, SliceOffset),
				ComputeShader,
				PassParams,
				GroupCount);

			AddEnqueueCopyPass(GraphBuilder, Readback.Get(), OutBuffer, (uint32)NumBytes);

			GraphBuilder.Execute();
		});

	// Blocking wait. This is an offline tool; a stall here is exactly what we
	// want, because it is also what keeps each dispatch inside the TDR window.
	FlushRenderingCommands();

	const double Deadline = FPlatformTime::Seconds() + 60.0;
	while (!Readback->IsReady())
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			OutError = TEXT("Timed out waiting for GPU readback. The driver may have reset (TDR); try a lower resolution or supersample count.");
			return false;
		}

		FPlatformProcess::Sleep(0.001f);
		FlushRenderingCommands();
	}

	bool bCopied = false;
	ENQUEUE_RENDER_COMMAND(NoiseBakeReadback)(
		[Readback, Destination, NumBytes, &bCopied](FRHICommandListImmediate&)
		{
			if (const void* Source = Readback->Lock((uint32)NumBytes))
			{
				FMemory::Memcpy(Destination->GetData(), Source, NumBytes);
				bCopied = true;
			}
			Readback->Unlock();
		});

	FlushRenderingCommands();

	if (!bCopied)
	{
		OutError = TEXT("GPU readback buffer could not be locked.");
		return false;
	}

	OutSlab = MoveTemp(*Destination);
	return true;
}

namespace
{
	/** Runs a whole volume in slabs, invoking OnSlab for each completed chunk. */
	bool RunFullVolume(
		const FNoiseBakeDispatchParams& Params,
		TFunctionRef<void(const TArray<FVector4f>&, int32 SliceOffset, int32 SliceCount)> OnSlab,
		FScopedSlowTask* Progress,
		FString& OutError)
	{
		const int32 Resolution = Params.Resolution;
		const int32 SlabDepth = NoiseBakeInternal::ComputeSlabDepth(Resolution, Params.Supersample);

		TArray<FVector4f> Slab;

		for (int32 SliceOffset = 0; SliceOffset < Resolution; SliceOffset += SlabDepth)
		{
			const int32 SliceCount = FMath::Min(SlabDepth, Resolution - SliceOffset);

			if (!FNoiseVolumeBaker::DispatchSlab(Params, SliceOffset, SliceCount, Slab, OutError))
			{
				return false;
			}

			OnSlab(Slab, SliceOffset, SliceCount);

			if (Progress)
			{
				Progress->EnterProgressFrame(
					(float)SliceCount / (float)Resolution,
					FText::Format(LOCTEXT("BakeSlice", "Baking slices {0} - {1}"),
						FText::AsNumber(SliceOffset),
						FText::AsNumber(SliceOffset + SliceCount)));
			}
		}

		return true;
	}
}

// ---------------------------------------------------------------------------
// Probe pass
// ---------------------------------------------------------------------------

bool FNoiseVolumeBaker::RunProbePass(
	const UNoiseBakeRecipe& Recipe,
	FNoiseBakeDispatchParams& InOutParams,
	TArray<FNoiseChannelNormalization>& OutNormalization,
	FString& OutError)
{
	TArray<FNoiseChannelRecipe> Channels;
	Recipe.GetChannels(Channels);

	OutNormalization.SetNum(4);

	const bool bNeedsProbe = Channels.ContainsByPredicate(
		[](const FNoiseChannelRecipe& C) { return C.NormalizeMode == ENoiseNormalizeMode::AutoProbe; });

	float ObservedMin[4] = { TNumericLimits<float>::Max(), TNumericLimits<float>::Max(), TNumericLimits<float>::Max(), TNumericLimits<float>::Max() };
	float ObservedMax[4] = { TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest() };

	if (bNeedsProbe)
	{
		// Half resolution, capped. Big enough that the finest octave is still
		// represented well enough for a range estimate, small enough to be
		// negligible against the main bake.
		int32 ProbeResolution = FMath::Clamp(Recipe.Resolution / 2, 32, 128);
		ProbeResolution = 1 << FMath::FloorLog2(ProbeResolution);

		FNoiseBakeDispatchParams ProbeParams = InOutParams;
		ProbeParams.Resolution = ProbeResolution;
		ProbeParams.Supersample = 1;
		ProbeParams.bApplyNormalize = false; // raw FBM output
		ProbeParams.DomainOffset = FVector3f::ZeroVector;

		auto Accumulate = [&ObservedMin, &ObservedMax](const TArray<FVector4f>& Slab, int32, int32)
		{
			for (const FVector4f& Texel : Slab)
			{
				ObservedMin[0] = FMath::Min(ObservedMin[0], Texel.X);
				ObservedMin[1] = FMath::Min(ObservedMin[1], Texel.Y);
				ObservedMin[2] = FMath::Min(ObservedMin[2], Texel.Z);
				ObservedMin[3] = FMath::Min(ObservedMin[3], Texel.W);

				ObservedMax[0] = FMath::Max(ObservedMax[0], Texel.X);
				ObservedMax[1] = FMath::Max(ObservedMax[1], Texel.Y);
				ObservedMax[2] = FMath::Max(ObservedMax[2], Texel.Z);
				ObservedMax[3] = FMath::Max(ObservedMax[3], Texel.W);
			}
		};

		if (!RunFullVolume(ProbeParams, Accumulate, nullptr, OutError))
		{
			return false;
		}
	}

	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FNoiseChannelRecipe& C = Channels[Index];

		float RangeMin = 0.0f;
		float RangeMax = 1.0f;

		switch (C.NormalizeMode)
		{
		case ENoiseNormalizeMode::AutoProbe:
		{
			RangeMin = ObservedMin[Index];
			RangeMax = ObservedMax[Index];

			const float Span = RangeMax - RangeMin;
			if (Span < KINDA_SMALL_NUMBER)
			{
				OutError = FString::Printf(
					TEXT("Channel %d produced a constant field (probe range %.6f to %.6f). "
						 "Check the basis and octave settings."),
					Index, RangeMin, RangeMax);
				return false;
			}

			// The probe undersamples, so nudge the range outward rather than
			// clipping the extremes in the full-resolution pass.
			const float Padding = Span * NoiseBakeInternal::ProbePadding;
			RangeMin -= Padding;
			RangeMax += Padding;
			break;
		}

		case ENoiseNormalizeMode::Manual:
			RangeMin = C.ManualMin;
			RangeMax = C.ManualMax;
			break;

		case ENoiseNormalizeMode::None:
		default:
			RangeMin = 0.0f;
			RangeMax = 1.0f;
			break;
		}

		const float Scale = 1.0f / FMath::Max(RangeMax - RangeMin, KINDA_SMALL_NUMBER);
		const float Bias = -RangeMin * Scale;

		InOutParams.ChannelNorm[Index] = FVector4f(Scale, Bias, 0.0f, 0.0f);

		OutNormalization[Index].ObservedMin = RangeMin;
		OutNormalization[Index].ObservedMax = RangeMax;
		OutNormalization[Index].Scale = Scale;
		OutNormalization[Index].Bias = Bias;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Tiling self-test
// ---------------------------------------------------------------------------

bool FNoiseVolumeBaker::RunTilingSelfTest(
	const FNoiseBakeDispatchParams& Params,
	int32 ProbeResolution,
	float Tolerance,
	FString& OutError)
{
	// The field is periodic with period exactly 1.0 in UVW, so evaluating at
	// UVW and UVW + 1 along any axis must agree. This tests the evaluator, not
	// the output image, which is the point: a subtly wrong modulo, or a warp
	// built from a non-periodic basis, produces a seam that a 2x2 tiled visual
	// check will happily hide but this catches immediately.

	FNoiseBakeDispatchParams TestParams = Params;
	TestParams.Resolution = ProbeResolution;
	TestParams.Supersample = 1;
	TestParams.bApplyNormalize = false;
	TestParams.DomainOffset = FVector3f::ZeroVector;

	TArray<FVector4f> Reference;
	if (!DispatchSlab(TestParams, 0, ProbeResolution, Reference, OutError))
	{
		return false;
	}

	static const FVector3f AxisOffsets[3] = {
		FVector3f(1.0f, 0.0f, 0.0f),
		FVector3f(0.0f, 1.0f, 0.0f),
		FVector3f(0.0f, 0.0f, 1.0f),
	};
	static const TCHAR* AxisNames[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		TestParams.DomainOffset = AxisOffsets[Axis];

		TArray<FVector4f> Shifted;
		if (!DispatchSlab(TestParams, 0, ProbeResolution, Shifted, OutError))
		{
			return false;
		}

		float MaxDelta = 0.0f;
		int32 WorstChannel = 0;

		for (int32 i = 0; i < Reference.Num(); ++i)
		{
			const FVector4f D = Reference[i] - Shifted[i];
			const float Deltas[4] = { FMath::Abs(D.X), FMath::Abs(D.Y), FMath::Abs(D.Z), FMath::Abs(D.W) };

			for (int32 c = 0; c < 4; ++c)
			{
				if (Deltas[c] > MaxDelta)
				{
					MaxDelta = Deltas[c];
					WorstChannel = c;
				}
			}
		}

		UE_LOG(LogNoiseVolumeBaker, Log,
			TEXT("Tiling self-test, %s axis: max delta %.9f (channel %d)"),
			AxisNames[Axis], MaxDelta, WorstChannel);

		if (MaxDelta > Tolerance)
		{
			OutError = FString::Printf(
				TEXT("Tiling self-test FAILED on the %s axis. Max difference between f(uvw) and f(uvw + 1) "
					 "was %.9f on channel %d, tolerance is %.9f. The generated volume would not tile."),
				AxisNames[Axis], MaxDelta, WorstChannel, Tolerance);
			return false;
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Quantization
// ---------------------------------------------------------------------------

void FNoiseVolumeBaker::QuantizeSlab(
	const TArray<FVector4f>& Slab,
	int32 Resolution,
	int32 SliceOffset,
	int32 SliceCount,
	bool bDither,
	TArray<uint8>& OutTexels)
{
	// TSF_BGRA8 source layout: byte order is B, G, R, A. The float4 carries
	// RGBA in channel order, so the write is deliberately swizzled here rather
	// than in the shader, keeping the shader's channel indices matching the
	// recipe's channel names.

	ParallelFor(SliceCount, [&](int32 LocalZ)
	{
		const int32 GlobalZ = SliceOffset + LocalZ;

		for (int32 Y = 0; Y < Resolution; ++Y)
		{
			const int64 RowSrc = ((int64)LocalZ * Resolution + Y) * Resolution;
			const int64 RowDst = (((int64)GlobalZ * Resolution + Y) * Resolution) * 4;

			for (int32 X = 0; X < Resolution; ++X)
			{
				const FVector4f& Texel = Slab[RowSrc + X];
				const float Values[4] = { Texel.X, Texel.Y, Texel.Z, Texel.W };

				uint8 Bytes[4];
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					float Scaled = FMath::Clamp(Values[Channel], 0.0f, 1.0f) * 255.0f;

					if (bDither)
					{
						Scaled += NoiseBakeInternal::TriangularDither(X, Y, GlobalZ, Channel);
					}

					Bytes[Channel] = (uint8)FMath::Clamp(FMath::RoundToInt(Scaled), 0, 255);
				}

				const int64 Dst = RowDst + (int64)X * 4;
				OutTexels[Dst + 0] = Bytes[2]; // B
				OutTexels[Dst + 1] = Bytes[1]; // G
				OutTexels[Dst + 2] = Bytes[0]; // R
				OutTexels[Dst + 3] = Bytes[3]; // A
			}
		}
	});
}

// ---------------------------------------------------------------------------
// Brick bounds
// ---------------------------------------------------------------------------

void FNoiseVolumeBaker::BuildBrickBounds(
	const TArray<uint8>& Texels,
	int32 Resolution,
	int32 BrickSize,
	FIntVector& OutDimensions,
	TArray<uint8>& OutBrickMinMax)
{
	const int32 BrickDim = Resolution / BrickSize;
	OutDimensions = FIntVector(BrickDim, BrickDim, BrickDim);

	const int64 NumBricks = (int64)BrickDim * BrickDim * BrickDim;
	OutBrickMinMax.SetNumUninitialized(NumBricks * 8);

	// Source byte order is BGRA; the brick record is written in RGBA order to
	// match the channel naming everywhere else.
	static const int32 SourceChannelForRGBA[4] = { 2, 1, 0, 3 };

	ParallelFor(BrickDim, [&](int32 BrickZ)
	{
		for (int32 BrickY = 0; BrickY < BrickDim; ++BrickY)
		{
			for (int32 BrickX = 0; BrickX < BrickDim; ++BrickX)
			{
				uint8 MinValue[4] = { 255, 255, 255, 255 };
				uint8 MaxValue[4] = { 0, 0, 0, 0 };

				for (int32 z = 0; z < BrickSize; ++z)
				{
					const int32 GlobalZ = BrickZ * BrickSize + z;

					for (int32 y = 0; y < BrickSize; ++y)
					{
						const int32 GlobalY = BrickY * BrickSize + y;
						const int64 RowBase = (((int64)GlobalZ * Resolution + GlobalY) * Resolution + (int64)BrickX * BrickSize) * 4;

						for (int32 x = 0; x < BrickSize; ++x)
						{
							const int64 Base = RowBase + (int64)x * 4;

							for (int32 Channel = 0; Channel < 4; ++Channel)
							{
								const uint8 Value = Texels[Base + SourceChannelForRGBA[Channel]];
								MinValue[Channel] = FMath::Min(MinValue[Channel], Value);
								MaxValue[Channel] = FMath::Max(MaxValue[Channel], Value);
							}
						}
					}
				}

				const int64 BrickIndex = ((int64)BrickZ * BrickDim + BrickY) * BrickDim + BrickX;
				const int64 Out = BrickIndex * 8;

				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					OutBrickMinMax[Out + Channel] = MinValue[Channel];
					OutBrickMinMax[Out + 4 + Channel] = MaxValue[Channel];
				}
			}
		}
	});
}

// ---------------------------------------------------------------------------
// Asset output
// ---------------------------------------------------------------------------

UVolumeTexture* FNoiseVolumeBaker::ResolveOrCreateTexture(UNoiseBakeRecipe& Recipe, FString& OutError)
{
	if (!Recipe.TargetTexture.IsNull())
	{
		if (UVolumeTexture* Existing = Recipe.TargetTexture.LoadSynchronous())
		{
			return Existing;
		}
	}

	const FString AssetName = Recipe.TargetAssetName.IsEmpty()
		? Recipe.GetName() + TEXT("_VT")
		: Recipe.TargetAssetName;

	const FString PackageName = Recipe.TargetPackagePath / AssetName;

	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = FString::Printf(
			TEXT("'%s' is not a valid package name. TargetPackagePath must be a content path such as /Game/Noise."),
			*PackageName);
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package '%s'."), *PackageName);
		return nullptr;
	}
	Package->FullyLoad();

	UVolumeTexture* Texture = NewObject<UVolumeTexture>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Texture)
	{
		OutError = FString::Printf(TEXT("Failed to create volume texture '%s'."), *AssetName);
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(Texture);
	Recipe.TargetTexture = Texture;

	return Texture;
}

bool FNoiseVolumeBaker::WriteTexture(
	UNoiseBakeRecipe& Recipe,
	UVolumeTexture& Texture,
	const TArray<uint8>& Texels,
	const TArray<FNoiseChannelNormalization>& Normalization,
	const FIntVector& BrickDimensions,
	const TArray<uint8>& BrickMinMax,
	FString& OutError)
{
	const int32 Resolution = Recipe.Resolution;

	Texture.PreEditChange(nullptr);

	// Source is the editor-facing representation and the only thing SavePackage
	// persists; PlatformData is derived from it. NumSlices carries the depth for
	// a volume texture.
	Texture.Source.Init(
		Resolution,
		Resolution,
		/*NumSlices=*/Resolution,
		/*NumMips=*/1,
		TSF_BGRA8,
		Texels.GetData());

	Texture.SRGB = false;
	Texture.CompressionSettings = TC_VectorDisplacementmap; // uncompressed BGRA8
	Texture.CompressionNone = true;
	Texture.MipGenSettings = Recipe.bGenerateMips ? TMGS_SimpleAverage : TMGS_NoMipmaps;
	Texture.Filter = TF_Trilinear;
	Texture.NeverStream = true;

	// Bake record. Written to the texture so a consumer can tell which bake it
	// is looking at without going back to the recipe.
	Texture.RemoveUserDataOfClass(UNoiseBakeAssetUserData::StaticClass());

	UNoiseBakeAssetUserData* UserData = NewObject<UNoiseBakeAssetUserData>(&Texture);
	UserData->BakeGuid = Recipe.LastBakeGuid;
	UserData->BakeVersion = Recipe.BakeVersion;
	UserData->Resolution = Resolution;
	UserData->ChannelNormalization = Normalization;

	TArray<FNoiseChannelRecipe> Channels;
	Recipe.GetChannels(Channels);
	UserData->ChannelBasePeriods.Reset(4);
	for (const FNoiseChannelRecipe& C : Channels)
	{
		UserData->ChannelBasePeriods.Add(C.BasePeriod);
	}

	if (Recipe.bBakeBrickBounds)
	{
		UserData->BrickSize = Recipe.BrickSize;
		UserData->BrickDimensions = BrickDimensions;
		UserData->BrickMinMax = BrickMinMax;
	}

	Texture.AddAssetUserData(UserData);

	// PostEditChange rebuilds platform data and calls UpdateResource. On a large
	// volume this is the slow part of the write, and it is unavoidable if the
	// asset is to be usable without a reload.
	Texture.PostEditChange();
	Texture.MarkPackageDirty();

	if (!Recipe.bSaveAfterBake)
	{
		return true;
	}

	UPackage* Package = Texture.GetOutermost();
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;

	if (!UPackage::SavePackage(Package, &Texture, *PackageFileName, SaveArgs))
	{
		OutError = FString::Printf(TEXT("SavePackage failed for '%s'."), *PackageFileName);
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

bool FNoiseVolumeBaker::Bake(UNoiseBakeRecipe* Recipe, FString& OutError)
{
	check(IsInGameThread());

	if (!Recipe)
	{
		OutError = TEXT("Null recipe.");
		return false;
	}

	if (!Recipe->Validate(OutError))
	{
		return false;
	}

	const double StartTime = FPlatformTime::Seconds();
	const int32 Resolution = Recipe->Resolution;

	FScopedSlowTask Progress(3.0f, LOCTEXT("BakingVolume", "Baking volume noise"));
	Progress.MakeDialog(/*bShowCancelButton=*/false);

	FNoiseBakeDispatchParams Params;
	BuildDispatchParams(*Recipe, Params);

	// -- Tiling self-test ---------------------------------------------------

	if (Recipe->bValidateTiling)
	{
		Progress.EnterProgressFrame(0.25f, LOCTEXT("ValidatingTiling", "Verifying periodicity"));

		if (!RunTilingSelfTest(Params, /*ProbeResolution=*/32, Recipe->TilingTolerance, OutError))
		{
			return false;
		}
	}

	// -- Probe --------------------------------------------------------------

	Progress.EnterProgressFrame(0.25f, LOCTEXT("Probing", "Probing value range"));

	TArray<FNoiseChannelNormalization> Normalization;
	if (!RunProbePass(*Recipe, Params, Normalization, OutError))
	{
		return false;
	}

	for (int32 Index = 0; Index < 4; ++Index)
	{
		UE_LOG(LogNoiseVolumeBaker, Log,
			TEXT("Channel %d normalization: range [%.6f, %.6f], scale %.6f, bias %.6f"),
			Index, Normalization[Index].ObservedMin, Normalization[Index].ObservedMax,
			Normalization[Index].Scale, Normalization[Index].Bias);
	}

	// -- Main bake ----------------------------------------------------------

	const int64 TotalBytes = (int64)Resolution * Resolution * Resolution * 4;

	TArray<uint8> Texels;
	Texels.SetNumUninitialized(TotalBytes);

	{
		FScopedSlowTask SliceProgress(1.0f, LOCTEXT("BakingSlices", "Evaluating volume"));

		const bool bDither = Recipe->bDither;

		auto OnSlab = [&Texels, Resolution, bDither](const TArray<FVector4f>& Slab, int32 SliceOffset, int32 SliceCount)
		{
			QuantizeSlab(Slab, Resolution, SliceOffset, SliceCount, bDither, Texels);
		};

		if (!RunFullVolume(Params, OnSlab, &SliceProgress, OutError))
		{
			return false;
		}
	}

	Progress.EnterProgressFrame(1.5f, LOCTEXT("WritingAsset", "Writing texture asset"));

	// -- Bricks -------------------------------------------------------------

	FIntVector BrickDimensions = FIntVector::ZeroValue;
	TArray<uint8> BrickMinMax;

	if (Recipe->bBakeBrickBounds)
	{
		BuildBrickBounds(Texels, Resolution, Recipe->BrickSize, BrickDimensions, BrickMinMax);
	}

	// -- Stamp and write ----------------------------------------------------

	Recipe->LastBakeGuid = FGuid::NewGuid();
	Recipe->BakeVersion++;
	Recipe->LastBakeNormalization = Normalization;

	UVolumeTexture* Texture = ResolveOrCreateTexture(*Recipe, OutError);
	if (!Texture)
	{
		return false;
	}

	if (!WriteTexture(*Recipe, *Texture, Texels, Normalization, BrickDimensions, BrickMinMax, OutError))
	{
		return false;
	}

	const double Duration = FPlatformTime::Seconds() - StartTime;

	Recipe->LastBakeStatus = FString::Printf(
		TEXT("Baked %d^3 to '%s' in %.2fs (version %d)."),
		Resolution, *Texture->GetName(), Duration, Recipe->BakeVersion);

	UE_LOG(LogNoiseVolumeBaker, Log, TEXT("%s"), *Recipe->LastBakeStatus);

	return true;
}

#undef LOCTEXT_NAMESPACE
