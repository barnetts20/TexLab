#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GasGiantSimTypes.h"
#include "GasGiantSimSubsystem.generated.h"

class FGasGiantSimulation;

/** Game-thread driver for the flow sim.
 *
 *  WHY A WORLD SUBSYSTEM AND NOT A SCENE VIEW EXTENSION.
 *
 *  A view extension runs inside the render pipeline, once per view. A planet's
 *  weather is world state, not view state: one planet has one flow field no
 *  matter how many viewports, reflection captures or PIE windows are looking at
 *  it, and a view extension would step the sim once for each of them. The
 *  result would be a sim whose rate depends on how many things are rendering,
 *  which is the kind of bug that only appears when someone opens a second
 *  viewport.
 *
 *  The cost is that the work is enqueued from the game tick rather than
 *  scheduled inside the render graph the scene is already building, so it lands
 *  in its own command list. At sub-millisecond that is not worth the
 *  correctness risk.
 *
 *  WHY THE CONFIG IS RE-READ EVERY TICK.
 *
 *  Because the point of this stage is watching the field. Every value in the
 *  config takes effect on the next frame, so the asset can be left open beside
 *  the debug target and tuned live. Only the grid dimensions are latched, and
 *  changing those reallocates and re-seeds rather than trying to resample a
 *  vorticity field onto a different grid. */
 /** BlueprintType is load-bearing, not decorative. K2Node_GetSubsystem only
  *  offers classes marked with it, so without it the "Get Gas Giant Sim
  *  Subsystem" node does not appear in the palette at all and every
  *  BlueprintCallable member below is unreachable -- present in the class,
  *  impossible to call. */
UCLASS(BlueprintType)
class NOISEBAKER_API UGasGiantSimSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// -- UWorldSubsystem ----------------------------------------------------

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// -- FTickableGameObject ------------------------------------------------

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	// -- Control ------------------------------------------------------------

	/** Begin stepping against this config. Safe to call again with a different
	 *  config; only a grid change forces a reseed. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	void StartSimulation(UGasGiantSimConfig* InConfig);

	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	void StopSimulation();

	/** Discard the field and re-seed from the current config. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	void ResetSimulation();

	/** Advance exactly N substeps and then pause. The single most useful thing
	 *  in here while bringing the solver up: watching one advection step at a
	 *  time in the residual view localises a discretisation bug in minutes. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	void StepOnce(int32 NumSteps = 1);

	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	bool IsSpinningUp() const { return StepsCompleted < SpinUpTarget; }

	/** Simulated time elapsed. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	float GetSimulatedTime() const { return SimulatedTime; }

	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	int32 GetStepsCompleted() const { return StepsCompleted; }

private:
	/** Builds the flat render-thread snapshot. Returns false if the config is
	 *  unusable, having already logged why. */
	bool BuildParams(FGasGiantSimParams& OutParams) const;

	/** Checks the render targets against the grid, reconfiguring them when
	 *  bAutoResizeTargets is set. Returns false if they remain unusable.
	 *
	 *  Validation before any dispatch, and all of it before any of it, for the
	 *  reason UNoiseBakeManifest validates a whole set before baking any of it:
	 *  a half-configured run is worse than a refused one, because it produces
	 *  output that looks like a result. */
	bool PrepareTargets() const;

	UPROPERTY(Transient)
	TObjectPtr<UGasGiantSimConfig> Config;

	FGasGiantSimulation* Simulation = nullptr;

	/** Consults UGasGiantSimSettings and starts if this world type wants it.
	 *  Run from the first Tick rather than Initialize, because the world is not
	 *  reliably ready to resolve a soft object reference that early. */
	void TryAutoStart();

	/** Logs the step size, Courant number and the TimeScale above which the sim
	 *  goes diffusive. Reported, never enforced -- see StepRatio. */
	void ReportCourant() const;

	bool bTriedAutoStart = false;

	/** Real time banked toward the next fixed substep. */
	float StepAccumulator = 0.0f;

	float SimulatedTime = 0.0f;
	int32 StepsCompleted = 0;

	/** Substeps to run before free-running. Set from SpinUpSteps at start. */
	int32 SpinUpTarget = 0;

	/** Manual steps queued by StepOnce, honoured even while paused. */
	int32 PendingManualSteps = 0;

	bool bRunning = false;
};