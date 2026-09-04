#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GasGiantSimTypes.generated.h"

class UVolumeTexture;
class UTextureRenderTarget2D;
class UTextureRenderTarget2DArray;

/** Which field the debug view renders. Mirrors GG_DEBUG_* in GasGiantSim.usf. */
UENUM(BlueprintType)
enum class EGasGiantDebugMode : uint8
{
	Vorticity   UMETA(DisplayName = "Vorticity"),
	Psi         UMETA(DisplayName = "Streamfunction"),
	Speed       UMETA(DisplayName = "Speed"),
	East        UMETA(DisplayName = "Eastward velocity"),
	North       UMETA(DisplayName = "Northward velocity"),

	/** Laplacian(psi) - omega. Should be featureless once converged; see the
	 *  note in MainDebugVisCS for how to read a residual that is not. */
	Residual    UMETA(DisplayName = "Poisson residual"),

	/** Zonal mean vorticity minus the prescribed target. Shows whether the
	 *  nudge is winning against the drag. */
	ZonalError  UMETA(DisplayName = "Zonal profile error"),
};

/** Per-layer multipliers on the shared jet profile.
 *
 *  THIS IS THE VERTICAL WIND SHEAR, and the reason the stack exists at all.
 *  Taylor-Proudman makes the flow invariant along the rotation axis, so columns
 *  move together and a full 3D solve is mostly wasted work. What a stack of 2D
 *  layers buys over a single layer is layers that DISAGREE.
 *
 *  Multipliers rather than independent profiles, deliberately. Independent
 *  profiles would let each layer put its jets at different latitudes, and the
 *  bands the material draws come from the shared profile, so they would then
 *  register with none of them. */
USTRUCT(BlueprintType)
struct FGasGiantLayerProfile
{
	GENERATED_BODY()

	/** Scales JetStrength. Below 1 gives a slower deep layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float JetScale = 1.0f;

	/** Scales EquatorialBoost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float BoostScale = 1.0f;

	/** Scales the stochastic forcing amplitude. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float ForcingScale = 1.0f;

	/** Scales the eddy drag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float DragScale = 1.0f;
};

/** Everything the sim needs, authored.
 *
 *  A DataAsset rather than component properties because at this stage the
 *  priority is watching the field rather than shipping a planet: the asset can
 *  be left open beside the debug target and edited while the sim runs, since
 *  every value here is re-read at the top of each frame. Nothing is latched at
 *  start except the grid dimensions.
 *
 *  The two render targets are AUTHORED ASSETS rather than transient textures
 *  created at runtime. That is the whole debugging story: point the sim at a
 *  render target asset, open it in the content browser, and watch. No material
 *  instance to plumb, no dynamic parameter to push, and any material can
 *  reference the flow target directly by asset path. */
UCLASS(BlueprintType)
class NOISEBAKER_API UGasGiantSimConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	// -- Grid ---------------------------------------------------------------

	/** Longitude columns. MUST BE EVEN: the polar fold in SimWrapCoord offsets
	 *  by exactly half the width, and an odd width lands the reflection half a
	 *  texel off, which shows as a faint discontinuity through both poles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "32", ClampMax = "2048"))
	int32 GridLongitude = 512;

	/** Latitude rows, in sin(latitude). Half the longitude count gives roughly
	 *  square cells in the tropics, which is where all the visible structure
	 *  is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "16", ClampMax = "1024"))
	int32 GridLatitude = 256;

	/** Stack depth. One layer works and has no vertical shear. Three is the
	 *  first count that gives a top, a middle and a bottom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LayerCount = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	TArray<FGasGiantLayerProfile> LayerProfiles;

	// -- Jet profile --------------------------------------------------------
	//
	// These are the SAME parameters the material's band profile uses and they
	// must be kept identical between the two, because GasGiantJets.ush is
	// shared and the bands are drawn from the same shape the jets are
	// maintained at. Divergence here means bands that do not sit on their jets.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile", meta = (ClampMin = "1.0"))
	float BandCount = 9.0f;

	/** Peak angular rate, radians per unit time on a unit sphere. Everything
	 *  in the sim is scaled against this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float JetStrength = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float EquatorialBoost = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float Asymmetry = 0.15f;

	/** x Flatness, y WidthBias, z ReliefThinning (material only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	FVector BandShape = FVector(0.6, 0.05, 1.0);

	// -- Physics ------------------------------------------------------------

	/** 2 * Omega. BETA IS NOT OPTIONAL and this is the number that supplies it.
	 *
	 *  Zero here means an isotropic inverse cascade, which merges eddies into
	 *  one hemispheric vortex rather than into jets. With NudgeRate high the
	 *  profile would still look correct while being maintained entirely by the
	 *  nudge, so this failure is invisible until the nudge comes down.
	 *
	 *  Wants to be well above JetStrength. The ratio sets the Rhines scale and
	 *  therefore how many bands emerge without being told to. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics")
	float PlanetaryVorticity = 4.0f;

	/** Simulated time per substep. Not a frame time.
	 *
	 *  The Courant limit is roughly GridLongitude * SimStepSize * JetStrength
	 *  over 2 pi; keep that under about a third. Semi-Lagrangian will not go
	 *  unstable above it, it will go DIFFUSIVE, which arrives at a bland field
	 *  quickly and looks like weak forcing rather than a step problem. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.0001"))
	float SimStepSize = 0.02f;

	/** Simulated time per second of real time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics")
	float TimeScale = 1.0f;

	/** Cap on substeps per frame, so a hitch does not cascade into a longer
	 *  hitch. Accumulated time beyond this is DISCARDED rather than carried,
	 *  because carrying it means a stall is followed by a burst of steps that
	 *  makes the next frame worse. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "1", ClampMax = "32"))
	int32 MaxSubstepsPerFrame = 4;

	// -- Forcing ------------------------------------------------------------

	/** Relaxation of the ZONAL MEAN toward the prescribed profile, per unit
	 *  time. Not of the field: nudging the field erases every eddy each step.
	 *
	 *  Start high. Prescribed and emergent jets are the same code path with
	 *  different coefficients, and a high nudge holds the profile still, which
	 *  is what lets advection and the Poisson solve be validated separately
	 *  against known answers. Walk it down afterwards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float NudgeRate = 2.0f;

	/** Stochastic small-scale forcing amplitude. The isotropic forcing the
	 *  banding physics assumes. Zero for the first bring-up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingAmplitude = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingScale = 8.0f;

	/** Drift through the forcing volume, which is what decorrelates it in
	 *  time. Zero makes the forcing static and the sim reaches a fixed point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	FVector ForcingDrift = FVector(0.03, 0.017, 0.023);

	/** Linear drag on the eddy vorticity. The large-scale sink that arrests
	 *  the inverse cascade; without it energy accumulates at the domain scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float DragRate = 0.05f;

	/** Relaxation between vertically adjacent layers. Weak on purpose: strong
	 *  coupling is the Taylor-Proudman limit, in which the stack collapses to
	 *  one layer and buys nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float LayerCoupling = 0.02f;

	// -- Polar filter -------------------------------------------------------

	/** cos(latitude) below which the longitudinal filter engages. 0.35 is
	 *  about 70 degrees. Above this nothing is filtered at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FilterLatitude = 0.35f;

	/** Bound on the filter width, so the innermost polar rows do not turn into
	 *  a loop over the whole grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "1", ClampMax = "256"))
	int32 FilterMaxHalfWidth = 48;

	// -- Solver -------------------------------------------------------------

	/** Red-black sweeps per substep. Each is two dispatches.
	 *
	 *  Small because the solve is WARM STARTED from the previous step's psi,
	 *  which is a near-solution. If the residual view shows structure that is
	 *  spread evenly rather than concentrated at the poles, this is the number
	 *  to raise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "1", ClampMax = "128"))
	int32 PoissonIterations = 8;

	/** Sweeps for the one cold start at init, which has no previous psi. Off
	 *  the frame budget, so it can afford to be generous. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 InitPoissonIterations = 256;

	/** Over-relaxation. 1 is plain Gauss-Seidel; the optimum for a problem
	 *  this size is near 1.8. AT OR ABOVE 2 THE ITERATION DIVERGES, and it does
	 *  so immediately rather than gradually, so a psi view that goes to solid
	 *  saturation on the first frame is almost always this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "0.1", ClampMax = "1.99"))
	float Relaxation = 1.8f;

	// -- Seed ---------------------------------------------------------------

	/** Band-limited tiling scalar noise. Read twice for different jobs: as the
	 *  seed eddy streamfunction at init, and as the stochastic forcing during
	 *  the run.
	 *
	 *  FEW OCTAVES. Vorticity is the Laplacian of this and a Laplacian is a
	 *  k^2 amplifier, so it is dominated by the FINEST scale present. A volume
	 *  carrying octaves down to the texel seeds grid-scale hash, which the
	 *  advection removes within a handful of steps, and the sim then arrives at
	 *  a bland zonal field looking exactly as though the seeding did nothing.
	 *  Resolution may go up freely now that this is off the per-pixel path;
	 *  octave count has to come down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Seed")
	TObjectPtr<UVolumeTexture> SeedVolume;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Seed", meta = (ClampMin = "0", ClampMax = "3"))
	int32 SeedChannel = 0;

	/** Amplitude of the seeded eddy streamfunction relative to the zonal one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Seed")
	float EddyAmplitude = 0.05f;

	/** Domain scale of the seed lookup, which sets the SIZE of the seeded
	 *  eddies. Should sit below the Rhines scale the jets imply, or the seed is
	 *  already larger than the flow can support and spin-up is spent taking it
	 *  apart rather than organising it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Seed")
	float SeedScale = 4.0f;

	// -- Spin-up ------------------------------------------------------------

	/** Substeps to run before the sim is considered ready.
	 *
	 *  Small because the expensive part of a real spin-up is skipped: what
	 *  takes thousands of turnovers is the cascade organising jets out of
	 *  isotropic forcing, and the jets here are prescribed. What is left is
	 *  eddies equilibrating with jets that already exist, which is tens of
	 *  turnovers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spin Up", meta = (ClampMin = "0", ClampMax = "8192"))
	int32 SpinUpSteps = 300;

	/** Spin-up substeps per frame.
	 *
	 *  Spread over frames rather than run in one graph, for two reasons. A
	 *  three-hundred-step graph is three thousand passes and will hitch. And
	 *  spreading it means the spin-up is WATCHABLE in the debug view, which is
	 *  where most of the diagnostic value is: seeing the seed organise, or fail
	 *  to, says far more than the converged state does. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spin Up", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxSpinUpStepsPerFrame = 8;

	// -- Targets ------------------------------------------------------------

	/** RGBA16F 2D array, sized (GridLongitude, GridLatitude, LayerCount).
	 *  RGB is tangent velocity as an angular rate, A is the streamfunction.
	 *  This is what the material samples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	TObjectPtr<UTextureRenderTarget2DArray> FlowTarget;

	/** Any 2D render target. Sized to the grid it is one texel per cell, which
	 *  is the intended setup. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	TObjectPtr<UTextureRenderTarget2D> DebugTarget;

	/** Reconfigure the targets to match the grid if they do not already.
	 *
	 *  On by default because the alternative during bring-up is a silent
	 *  no-op: a mismatched target is refused, and a refused sim looks exactly
	 *  like a sim that is running and producing nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	bool bAutoResizeTargets = true;

	// -- Debug --------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	EGasGiantDebugMode DebugMode = EGasGiantDebugMode::Vorticity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0", ClampMax = "7"))
	int32 DebugLayer = 0;

	/** Value mapped to full colour. Everything shown is signed and O(1) against
	 *  PlanetaryVorticity, so start near that. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.0001"))
	float DebugScale = 4.0f;

	/** Halt stepping without tearing the state down. The debug view keeps
	 *  updating, so a frozen field can still be inspected in every mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bPaused = false;
};

/** Flat, POD-ish snapshot handed to the render thread.
 *
 *  Deliberately plain, for exactly the reason FNoiseBakeDispatchParams is: it
 *  gets captured by value into a render command, and touching a UObject from
 *  the render thread is a crash waiting for a garbage collection to schedule
 *  itself badly. Everything the render thread needs is copied here on the game
 *  thread, including the seed volume's RHI reference. */
struct FGasGiantSimParams
{
	FIntVector GridSize = FIntVector(512, 256, 3);

	FVector4f JetParams = FVector4f(9.0f, 0.1f, 0.4f, 0.15f);
	FVector4f BandShape = FVector4f(0.6f, 0.05f, 1.0f, 0.0f);
	FVector4f LayerProfile[8];

	float DeltaTime = 0.02f;
	float Time = 0.0f;
	float PlanetaryVorticity = 4.0f;

	int32 SeedChannel = 0;
	float EddyAmplitude = 0.05f;
	float SeedScale = 4.0f;

	float NudgeRate = 2.0f;
	float ForcingAmplitude = 0.0f;
	float ForcingScale = 8.0f;
	FVector3f ForcingDrift = FVector3f(0.03f, 0.017f, 0.023f);
	float DragRate = 0.05f;
	float LayerCoupling = 0.02f;

	float FilterLatitude = 0.35f;
	int32 FilterMaxHalfWidth = 48;

	int32 PoissonIterations = 8;
	float Relaxation = 1.8f;

	int32 DebugMode = 0;
	int32 DebugLayer = 0;
	float DebugScale = 4.0f;
	FIntPoint DebugSize = FIntPoint::ZeroValue;

	/** Held as RHI references so the render thread never dereferences a
	 *  UObject. Null seed is legal and evaluates as zero, which is a valid if
	 *  uninteresting state and is better than refusing to run. */
	FTextureRHIRef SeedTexture;
	FTextureRHIRef FlowTexture;
	FTextureRHIRef DebugTexture;
};
