#include "GasGiantSimSubsystem.h"

#include "GasGiantSimulation.h"
#include "GasGiantSimSettings.h"
#include "Engine/VolumeTexture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"

// TAutoConsoleVariable and FAutoConsoleCommandWithWorldAndArgs.
#include "HAL/IConsoleManager.h"

// UWorld::GetSubsystem, used by the console commands to find the right
// per-world instance.
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogGasGiant, Log, All);

// ---------------------------------------------------------------------------
// Console commands. Present because the fastest debugging loop for a field
// like this is: change one thing, look, change it back. Going through a
// blueprint or a details panel for that is enough friction to discourage it.
// ---------------------------------------------------------------------------

static TAutoConsoleVariable<int32> CVarGasGiantDebugMode(
	TEXT("r.GasGiant.DebugMode"),
	-1,
	TEXT("Override the config's debug view. -1 uses the config.\n")
	TEXT("0 Vorticity, 1 Streamfunction, 2 Speed, 3 East, 4 North,\n")
	TEXT("5 Poisson residual, 6 Zonal profile error."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGasGiantDebugLayer(
	TEXT("r.GasGiant.DebugLayer"),
	-1,
	TEXT("Override the debug layer. -1 uses the config."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGasGiantDebugScale(
	TEXT("r.GasGiant.DebugScale"),
	-1.0f,
	TEXT("Override the debug value scale. Negative uses the config."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGasGiantPaused(
	TEXT("r.GasGiant.Paused"),
	-1,
	TEXT("Override pause. -1 uses the config, 0 runs, 1 freezes."),
	ECVF_RenderThreadSafe);

// ---------------------------------------------------------------------------
// Console commands.
//
// FConsoleCommandWithWorldAndArgsDelegate rather than a plain command, because
// the subsystem is per world and a static command has no other way to find the
// right one. The world it hands back is the one the command was issued in,
// which is the editor world when typed into the editor console and the PIE
// world when typed during play -- exactly the disambiguation wanted.
// ---------------------------------------------------------------------------

namespace
{
	UGasGiantSimSubsystem* FindSubsystem(UWorld* World)
	{
		return World ? World->GetSubsystem<UGasGiantSimSubsystem>() : nullptr;
	}

	/** Resolves an optional asset path argument, falling back to the project
	 *  setting. Both paths are logged, because "started against the wrong
	 *  config" and "did not start" look identical from the debug view. */
	UGasGiantSimConfig* ResolveConfig(const TArray<FString>& Args)
	{
		if (Args.Num() > 0)
		{
			UGasGiantSimConfig* Loaded = LoadObject<UGasGiantSimConfig>(nullptr, *Args[0]);

			if (!Loaded)
			{
				UE_LOG(LogGasGiant, Error, TEXT("No GasGiantSimConfig at '%s'."), *Args[0]);
			}

			return Loaded;
		}

		const UGasGiantSimSettings* Settings = GetDefault<UGasGiantSimSettings>();

		if (!Settings || Settings->DefaultConfig.IsNull())
		{
			UE_LOG(LogGasGiant, Error,
				TEXT("No config given and no DefaultConfig set in ")
				TEXT("Project Settings -> Plugins -> Gas Giant Sim."));
			return nullptr;
		}

		return Settings->DefaultConfig.LoadSynchronous();
	}
}

static FAutoConsoleCommandWithWorldAndArgs GGasGiantStartCmd(
	TEXT("GasGiant.Start"),
	TEXT("Start the flow sim. Optional argument is a GasGiantSimConfig asset path; ")
	TEXT("with none, uses the DefaultConfig from Project Settings."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				if (UGasGiantSimConfig* Config = ResolveConfig(Args))
				{
					Sub->StartSimulation(Config);
					UE_LOG(LogGasGiant, Log, TEXT("Started against '%s'."), *Config->GetName());
				}
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GGasGiantStopCmd(
	TEXT("GasGiant.Stop"),
	TEXT("Stop stepping. State is kept, so GasGiant.Start resumes rather than reseeds."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>&, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				Sub->StopSimulation();
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GGasGiantResetCmd(
	TEXT("GasGiant.Reset"),
	TEXT("Discard the field and reseed from the current config. Also the way to ")
	TEXT("pick up a changed grid size or seed volume."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>&, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				Sub->ResetSimulation();
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GGasGiantStepCmd(
	TEXT("GasGiant.Step"),
	TEXT("Advance N substeps while paused. Default 1."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				Sub->StepOnce(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GGasGiantStatusCmd(
	TEXT("GasGiant.Status"),
	TEXT("Report step count, simulated time and spin-up progress."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>&, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				UE_LOG(LogGasGiant, Display,
					TEXT("steps %d, simulated time %.2f, %s"),
					Sub->GetStepsCompleted(),
					Sub->GetSimulatedTime(),
					Sub->IsSpinningUp() ? TEXT("spinning up") : TEXT("free running"));
			}
		}));

// ---------------------------------------------------------------------------

void UGasGiantSimSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Simulation = new FGasGiantSimulation();
}

void UGasGiantSimSubsystem::Deinitialize()
{
	if (Simulation)
	{
		// The render thread owns the pooled allocations, so they are released
		// there and the flush is what makes deleting the object afterwards
		// safe. Deleting from the game thread without this races a frame that
		// is already referencing them.
		FGasGiantSimulation* Sim = Simulation;
		Simulation = nullptr;

		ENQUEUE_RENDER_COMMAND(GasGiantRelease)(
			[Sim](FRHICommandListImmediate&)
			{
				Sim->Release_RenderThread();
				delete Sim;
			});

		FlushRenderingCommands();
	}

	Super::Deinitialize();
}

bool UGasGiantSimSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

TStatId UGasGiantSimSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGasGiantSimSubsystem, STATGROUP_Tickables);
}

void UGasGiantSimSubsystem::StartSimulation(UGasGiantSimConfig* InConfig)
{
	if (!InConfig)
	{
		UE_LOG(LogGasGiant, Warning, TEXT("StartSimulation called with a null config."));
		return;
	}

	Config = InConfig;
	bRunning = true;

	StepAccumulator = 0.0f;
	SimulatedTime = 0.0f;
	StepsCompleted = 0;
	SpinUpTarget = FMath::Max(InConfig->SpinUpSteps, 0);
	PendingManualSteps = 0;

	ResetSimulation();
}

void UGasGiantSimSubsystem::StopSimulation()
{
	bRunning = false;
}

void UGasGiantSimSubsystem::ResetSimulation()
{
	SimulatedTime = 0.0f;
	StepsCompleted = 0;
	StepAccumulator = 0.0f;

	if (Simulation)
	{
		FGasGiantSimulation* Sim = Simulation;

		ENQUEUE_RENDER_COMMAND(GasGiantReset)(
			[Sim](FRHICommandListImmediate&)
			{
				Sim->RequestReset();
			});
	}
}

void UGasGiantSimSubsystem::StepOnce(int32 NumSteps)
{
	PendingManualSteps += FMath::Max(NumSteps, 1);
}

bool UGasGiantSimSubsystem::PrepareTargets() const
{
	if (!Config)
	{
		return false;
	}

	const int32 W = Config->GridLongitude;
	const int32 H = Config->GridLatitude;
	const int32 Slices = FMath::Clamp(Config->LayerCount, 1, 8);

	// -- Flow target --------------------------------------------------------

	UTextureRenderTarget2DArray* Flow = Config->FlowTarget;

	if (!Flow)
	{
		UE_LOG(LogGasGiant, Error,
			TEXT("No FlowTarget set. Create a Texture Render Target 2D Array asset, ")
			TEXT("set it here, and the sim will size it automatically."));
		return false;
	}

	const bool bFlowMismatch =
		Flow->SizeX != W ||
		Flow->SizeY != H ||
		Flow->Slices != Slices ||
		Flow->OverrideFormat != PF_FloatRGBA ||
		!Flow->bCanCreateUAV;

	if (bFlowMismatch)
	{
		if (!Config->bAutoResizeTargets)
		{
			UE_LOG(LogGasGiant, Error,
				TEXT("FlowTarget is %dx%dx%d, needs %dx%dx%d RGBA16F with bCanCreateUAV. ")
				TEXT("Enable bAutoResizeTargets or fix the asset."),
				Flow->SizeX, Flow->SizeY, Flow->Slices, W, H, Slices);
			return false;
		}

		// bCanCreateUAV must be set BEFORE the resource is created, or the
		// texture comes back without UAV support and every dispatch that writes
		// it silently does nothing. That failure presents as a black flow
		// texture with no warning anywhere, which is why it is set here rather
		// than left to the asset.
		Flow->bCanCreateUAV = true;
		Flow->OverrideFormat = PF_FloatRGBA;
		Flow->ClearColor = FLinearColor::Black;
		Flow->Init(W, H, Slices, PF_FloatRGBA);
		Flow->UpdateResourceImmediate(true);

		UE_LOG(LogGasGiant, Log, TEXT("Resized FlowTarget to %dx%d x %d slices."), W, H, Slices);
	}

	// -- Debug target -------------------------------------------------------

	if (UTextureRenderTarget2D* Debug = Config->DebugTarget)
	{
		const bool bDebugMismatch =
			Debug->SizeX != W ||
			Debug->SizeY != H ||
			!Debug->bCanCreateUAV;

		if (bDebugMismatch && Config->bAutoResizeTargets)
		{
			Debug->bCanCreateUAV = true;
			Debug->ClearColor = FLinearColor::Black;
			Debug->InitCustomFormat(W, H, PF_FloatRGBA, /*bForceLinearGamma*/ true);
			Debug->UpdateResourceImmediate(true);

			UE_LOG(LogGasGiant, Log, TEXT("Resized DebugTarget to %dx%d."), W, H);
		}
	}

	return true;
}

bool UGasGiantSimSubsystem::BuildParams(FGasGiantSimParams& Out) const
{
	if (!Config)
	{
		return false;
	}

	// Longitude must be even: the polar fold in SimWrapCoord offsets by exactly
	// half the width. Rounded down rather than refused, since the alternative
	// is a stalled sim over a parameter nobody would think to check.
	const int32 W = FMath::Max(Config->GridLongitude & ~1, 32);
	const int32 H = FMath::Max(Config->GridLatitude, 16);
	const int32 Slices = FMath::Clamp(Config->LayerCount, 1, 8);

	Out.GridSize = FIntVector(W, H, Slices);

	Out.JetParams = FVector4f(
		Config->BandCount,
		Config->JetStrength,
		Config->EquatorialBoost,
		Config->Asymmetry);

	Out.BandShape = FVector4f(
		(float)Config->BandShape.X,
		(float)Config->BandShape.Y,
		(float)Config->BandShape.Z,
		0.0f);

	for (int32 i = 0; i < 8; ++i)
	{
		// Layers past the authored list fall back to an unscaled copy of the
		// shared profile rather than to zero. Zero would give a layer with no
		// jets at all, which reads as a bug in the sim rather than as a missing
		// array entry.
		const FGasGiantLayerProfile P = Config->LayerProfiles.IsValidIndex(i)
			? Config->LayerProfiles[i]
			: FGasGiantLayerProfile();

		Out.LayerProfile[i] = FVector4f(P.JetScale, P.BoostScale, P.ForcingScale, P.DragScale);
	}

	Out.DeltaTime = FMath::Max(Config->SimStepSize, 1e-4f);
	Out.Time = SimulatedTime;
	Out.PlanetaryVorticity = Config->PlanetaryVorticity;

	Out.SeedChannel = FMath::Clamp(Config->SeedChannel, 0, 3);
	Out.EddyAmplitude = Config->EddyAmplitude;
	Out.SeedScale = Config->SeedScale;

	Out.NudgeRate = Config->NudgeRate;
	Out.ForcingAmplitude = Config->ForcingAmplitude;
	Out.ForcingScale = Config->ForcingScale;
	Out.ForcingDrift = FVector3f(Config->ForcingDrift);
	Out.DragRate = Config->DragRate;
	Out.LayerCoupling = Config->LayerCoupling;

	Out.FilterLatitude = FMath::Clamp(Config->FilterLatitude, 0.0f, 1.0f);
	Out.FilterMaxHalfWidth = FMath::Clamp(Config->FilterMaxHalfWidth, 1, 256);

	Out.PoissonIterations = FMath::Clamp(Config->PoissonIterations, 1, 128);

	// Hard clamp below 2. At or above it the SOR iteration diverges
	// immediately, and the symptom -- psi saturating on the first frame -- is
	// far enough from the cause to be worth making unreachable.
	Out.Relaxation = FMath::Clamp(Config->Relaxation, 0.1f, 1.99f);

	const int32 ModeOverride = CVarGasGiantDebugMode.GetValueOnGameThread();
	const int32 LayerOverride = CVarGasGiantDebugLayer.GetValueOnGameThread();
	const float ScaleOverride = CVarGasGiantDebugScale.GetValueOnGameThread();

	Out.DebugMode = (ModeOverride >= 0) ? ModeOverride : (int32)Config->DebugMode;
	Out.DebugLayer = FMath::Clamp((LayerOverride >= 0) ? LayerOverride : Config->DebugLayer, 0, Slices - 1);
	Out.DebugScale = (ScaleOverride > 0.0f) ? ScaleOverride : Config->DebugScale;

	// -- Resource handles ---------------------------------------------------

	if (Config->SeedVolume && Config->SeedVolume->GetResource())
	{
		Out.SeedTexture = Config->SeedVolume->GetResource()->TextureRHI;
	}

	if (Config->FlowTarget)
	{
		if (FTextureRenderTargetResource* Res = Config->FlowTarget->GameThread_GetRenderTargetResource())
		{
			Out.FlowTexture = Res->GetRenderTargetTexture();
		}
	}

	if (Config->DebugTarget)
	{
		if (FTextureRenderTargetResource* Res = Config->DebugTarget->GameThread_GetRenderTargetResource())
		{
			Out.DebugTexture = Res->GetRenderTargetTexture();
			Out.DebugSize = FIntPoint(Config->DebugTarget->SizeX, Config->DebugTarget->SizeY);
		}
	}

	return Out.FlowTexture.IsValid();
}

void UGasGiantSimSubsystem::TryAutoStart()
{
	const UGasGiantSimSettings* Settings = GetDefault<UGasGiantSimSettings>();

	if (!Settings || Settings->DefaultConfig.IsNull())
	{
		return;
	}

	const UWorld* World = GetWorld();

	if (!World)
	{
		return;
	}

	const bool bEditorWorld = (World->WorldType == EWorldType::Editor);
	const bool bWanted = bEditorWorld ? Settings->bAutoStartInEditor : Settings->bAutoStartInGame;

	if (!bWanted)
	{
		return;
	}

	if (UGasGiantSimConfig* Loaded = Settings->DefaultConfig.LoadSynchronous())
	{
		UE_LOG(LogGasGiant, Log, TEXT("Auto-starting against '%s' in %s world."),
			*Loaded->GetName(), bEditorWorld ? TEXT("editor") : TEXT("game"));

		StartSimulation(Loaded);
	}
}

void UGasGiantSimSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Once, on the first tick rather than in Initialize: resolving a soft
	// object reference during subsystem construction can run before the asset
	// registry is usable, and a failed load there is silent.
	if (!bTriedAutoStart)
	{
		bTriedAutoStart = true;
		TryAutoStart();
	}

	if (!bRunning || !Config || !Simulation)
	{
		return;
	}

	if (!PrepareTargets())
	{
		return;
	}

	const int32 PauseOverride = CVarGasGiantPaused.GetValueOnGameThread();
	const bool bPaused = (PauseOverride >= 0) ? (PauseOverride != 0) : Config->bPaused;

	// -- Decide how many substeps this frame --------------------------------

	int32 Substeps = 0;

	const float StepSize = FMath::Max(Config->SimStepSize, 1e-4f);

	if (StepsCompleted < SpinUpTarget)
	{
		// SPIN-UP IS SPREAD OVER FRAMES, not run in one graph.
		//
		// Three hundred substeps is three thousand passes, which will hitch
		// visibly and may trip the driver's timeout. Spreading it also makes
		// the spin-up WATCHABLE, and that is where most of the diagnostic value
		// is: seeing whether the seed organises tells you more than the
		// converged state does, because a converged-looking field can be
		// converged for the wrong reason.
		Substeps = FMath::Min(
			FMath::Max(Config->MaxSpinUpStepsPerFrame, 1),
			SpinUpTarget - StepsCompleted);
	}
	else if (PendingManualSteps > 0)
	{
		Substeps = FMath::Min(PendingManualSteps, FMath::Max(Config->MaxSubstepsPerFrame, 1));
		PendingManualSteps -= Substeps;
	}
	else if (!bPaused)
	{
		StepAccumulator += DeltaTime * Config->TimeScale;

		Substeps = FMath::FloorToInt(StepAccumulator / StepSize);
		Substeps = FMath::Min(Substeps, FMath::Max(Config->MaxSubstepsPerFrame, 1));

		// Consume only what was taken, then DISCARD the rest of the bank if it
		// exceeded the cap. Carrying it means a hitch is followed by a burst of
		// catch-up steps that makes the next frame worse, and on a heavily
		// loaded frame that spirals. Simulated time falling behind real time
		// during a stall is the correct behaviour for a visual effect.
		StepAccumulator -= Substeps * StepSize;
		StepAccumulator = FMath::Min(StepAccumulator, StepSize);
	}

	FGasGiantSimParams Params;
	if (!BuildParams(Params))
	{
		return;
	}

	// Time advances on the game thread so the forcing drift is consistent with
	// the step count regardless of when the render thread gets to it.
	SimulatedTime += Substeps * StepSize;
	StepsCompleted += Substeps;

	FGasGiantSimulation* Sim = Simulation;

	// Zero substeps still enqueues. The debug view must keep updating on a
	// paused or fully spun-up-and-idle sim, or switching debug modes while
	// paused would appear to do nothing.
	ENQUEUE_RENDER_COMMAND(GasGiantStep)(
		[Sim, Params, Substeps](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			Sim->Enqueue_RenderThread(GraphBuilder, Params, Substeps);

			GraphBuilder.Execute();
		});
}