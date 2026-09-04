#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NoiseBakeManifest.generated.h"

class UNoiseBakeRecipeBase;

/** Outcome for one recipe in a batch run. Recorded, never authored. */
USTRUCT(BlueprintType)
struct NOISEBAKEREDITOR_API FNoiseBakeBatchResult
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FString Recipe;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	bool bSucceeded = false;

	/** The recipe's own bake status on success, or the failure reason. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	FString Message;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Result")
	float Seconds = 0.0f;
};

/** A set of recipes baked together.
 *
 *  Exists because noise content ships as a SET. A library plugin's textures are
 *  authored against each other -- a flow field and the detail layer that rides
 *  it are only correct at the scales they were tuned at together -- so baking
 *  them one asset at a time invites a half-updated set that looks subtly wrong
 *  with nothing to point at.
 *
 *  Two ways to name the members, and they compose:
 *
 *    Recipes      An explicit list. Order-independent and survives assets being
 *                 moved, since soft pointers are fixed up.
 *
 *    SearchPaths  Folders, resolved at BAKE time rather than stored. A recipe
 *                 added to a covered folder is picked up on the next run with
 *                 no edit here, which is the property that makes this usable as
 *                 a plugin's build step.
 *
 *  The union is deduplicated and sorted by package path, so a run is
 *  deterministic and its log is diffable between runs. */
UCLASS(BlueprintType)
class NOISEBAKEREDITOR_API UNoiseBakeManifest : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Recipes named directly. Combined with anything found under SearchPaths. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contents")
	TArray<TSoftObjectPtr<UNoiseBakeRecipeBase>> Recipes;

	/** Content folders to scan. Any recipe class is matched, including
	 *  subclasses, so a folder entry does not go stale when a new recipe type
	 *  is added to the plugin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contents", meta = (ContentDir))
	TArray<FDirectoryPath> SearchPaths;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contents")
	bool bRecursive = true;

	// -- Behaviour ----------------------------------------------------------

	/** Validate every recipe before baking any of them.
	 *
	 *  On by default, and the default is the point. Baking as you go means a
	 *  bad recipe at position 13 leaves twelve textures written and the set
	 *  inconsistent, with the failure reported long after the damage. Checking
	 *  first makes authoring errors all-or-nothing.
	 *
	 *  It cannot make the run atomic -- a GPU or package-write failure can
	 *  still land mid-batch -- but those are not authoring mistakes and are not
	 *  what this guards against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Behaviour")
	bool bValidateAllBeforeBaking = true;

	/** Keep going after a bake fails rather than stopping at the first one.
	 *
	 *  Useful when a run is expected to be partly broken and you want the whole
	 *  list of failures in one pass instead of one per attempt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Behaviour")
	bool bContinueOnFailure = false;

	// -- Run record ---------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Last Run")
	FString Summary;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Last Run")
	TArray<FNoiseBakeBatchResult> Results;

	// -- Actions ------------------------------------------------------------

	UFUNCTION(CallInEditor, Category = "Bake", meta = (DisplayName = "Bake All"))
	void BakeAll();

	/** Validates without baking. Same resolution and ordering as a real run, so
	 *  it is a genuine dry run rather than an approximation of one. */
	UFUNCTION(CallInEditor, Category = "Bake", meta = (DisplayName = "Validate All"))
	void ValidateAll();

	/** Lists what a run would cover, without validating or baking. For checking
	 *  that a SearchPath matches what you expected before committing to it. */
	UFUNCTION(CallInEditor, Category = "Bake", meta = (DisplayName = "List Contents"))
	void ListContents();

	/** Resolved union of Recipes and SearchPaths, deduplicated and sorted by
	 *  package path. Loads soft references. */
	UFUNCTION(BlueprintCallable, Category = "Bake")
	TArray<UNoiseBakeRecipeBase*> ResolveRecipes() const;

private:
	/** Shared body for BakeAll and ValidateAll. */
	void Run(bool bBake);
};
