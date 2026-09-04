#include "NoiseBakeManifest.h"

#include "NoiseBakeRecipeBase.h"
#include "NoiseVolumeBaker.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"

#define LOCTEXT_NAMESPACE "NoiseBakeManifest"

DEFINE_LOG_CATEGORY_STATIC(LogNoiseBakeManifest, Log, All);

TArray<UNoiseBakeRecipeBase*> UNoiseBakeManifest::ResolveRecipes() const
{
	TArray<UNoiseBakeRecipeBase*> Resolved;
	TSet<const UNoiseBakeRecipeBase*> Seen;

	auto Add = [&Resolved, &Seen](UNoiseBakeRecipeBase* Recipe)
		{
			// Dedup by resolved object rather than by path. The same asset can
			// arrive once from Recipes and once from a SearchPath that covers
			// it, and baking it twice would burn the time and bump BakeVersion
			// by two for no reason.
			if (Recipe && !Seen.Contains(Recipe))
			{
				Seen.Add(Recipe);
				Resolved.Add(Recipe);
			}
		};

	for (const TSoftObjectPtr<UNoiseBakeRecipeBase>& Soft : Recipes)
	{
		if (Soft.IsNull())
		{
			continue;
		}

		// Synchronous, deliberately. This is an editor action the user just
		// pressed, and an async load would report an empty manifest for a set
		// that is merely not resident yet.
		Add(Soft.LoadSynchronous());
	}

	if (SearchPaths.Num() > 0)
	{
		const FAssetRegistryModule& Module =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		IAssetRegistry& Registry = Module.Get();

		TArray<FString> Paths;
		for (const FDirectoryPath& Dir : SearchPaths)
		{
			if (!Dir.Path.IsEmpty())
			{
				Paths.Add(Dir.Path);
			}
		}

		if (Paths.Num() > 0)
		{
			// A folder that has never been browsed may not be in the registry
			// yet, and the failure mode without this is an empty result that
			// looks exactly like a wrong path.
			Registry.ScanPathsSynchronous(Paths, /*bForceRescan*/ false);

			FARFilter Filter;
			Filter.ClassPaths.Add(UNoiseBakeRecipeBase::StaticClass()->GetClassPathName());

			// Matches subclasses, so a folder entry does not need revisiting
			// when a new recipe type is added.
			Filter.bRecursiveClasses = true;
			Filter.bRecursivePaths = bRecursive;

			for (const FString& Path : Paths)
			{
				Filter.PackagePaths.Add(FName(*Path));
			}

			TArray<FAssetData> Found;
			Registry.GetAssets(Filter, Found);

			for (const FAssetData& Asset : Found)
			{
				Add(Cast<UNoiseBakeRecipeBase>(Asset.GetAsset()));
			}
		}
	}

	// Sorted by package path so a run is deterministic and its log diffs
	// cleanly against the previous one. Registry order is not stable across
	// sessions and neither is array order after assets are moved.
	Resolved.Sort([](const UNoiseBakeRecipeBase& A, const UNoiseBakeRecipeBase& B)
		{
			return A.GetPathName() < B.GetPathName();
		});

	return Resolved;
}

void UNoiseBakeManifest::ListContents()
{
	const TArray<UNoiseBakeRecipeBase*> Batch = ResolveRecipes();

	Results.Reset(Batch.Num());

	for (const UNoiseBakeRecipeBase* Recipe : Batch)
	{
		FNoiseBakeBatchResult Entry;
		Entry.Recipe = Recipe->GetName();
		Entry.bSucceeded = true;
		Entry.Message = FString::Printf(
			TEXT("%d^3, %s"), Recipe->Resolution,
			*Recipe->GetPathName());

		Results.Add(Entry);
	}

	Summary = FString::Printf(TEXT("%d recipe(s) in scope."), Batch.Num());

	UE_LOG(LogNoiseBakeManifest, Log, TEXT("[%s] %s"), *GetName(), *Summary);

	MarkPackageDirty();
}

void UNoiseBakeManifest::ValidateAll()
{
	Run(/*bBake*/ false);
}

void UNoiseBakeManifest::BakeAll()
{
	Run(/*bBake*/ true);
}

void UNoiseBakeManifest::Run(bool bBake)
{
	const TArray<UNoiseBakeRecipeBase*> Batch = ResolveRecipes();

	Results.Reset(Batch.Num());

	if (Batch.Num() == 0)
	{
		Summary = TEXT("Nothing in scope. Check Recipes and SearchPaths.");
		UE_LOG(LogNoiseBakeManifest, Warning, TEXT("[%s] %s"), *GetName(), *Summary);
		MarkPackageDirty();
		return;
	}

	const double StartTime = FPlatformTime::Seconds();

	// -- Validation pass ----------------------------------------------------
	//
	// Runs for ValidateAll always, and for BakeAll when bValidateAllBeforeBaking
	// is set. Both walk the same resolved list in the same order, so a dry run
	// is a genuine rehearsal rather than an approximation of one.

	int32 Failures = 0;

	if (!bBake || bValidateAllBeforeBaking)
	{
		FScopedSlowTask Task((float)Batch.Num(), LOCTEXT("Validating", "Validating recipes"));
		Task.MakeDialog(/*bShowCancelButton*/ true);

		for (UNoiseBakeRecipeBase* Recipe : Batch)
		{
			if (Task.ShouldCancel())
			{
				break;
			}

			Task.EnterProgressFrame(1.0f, FText::FromString(Recipe->GetName()));

			FString Error;
			const bool bValid = Recipe->Validate(Error);

			if (!bValid)
			{
				Failures++;

				UE_LOG(LogNoiseBakeManifest, Error,
					TEXT("[%s] %s failed validation: %s"), *GetName(), *Recipe->GetName(), *Error);
			}

			if (!bBake)
			{
				FNoiseBakeBatchResult Entry;
				Entry.Recipe = Recipe->GetName();
				Entry.bSucceeded = bValid;
				Entry.Message = bValid ? TEXT("Validation passed.") : Error;

				Results.Add(Entry);
			}
		}

		if (!bBake)
		{
			Summary = FString::Printf(
				TEXT("Validated %d recipe(s): %d passed, %d failed."),
				Batch.Num(), Batch.Num() - Failures, Failures);

			UE_LOG(LogNoiseBakeManifest, Log, TEXT("[%s] %s"), *GetName(), *Summary);
			MarkPackageDirty();
			return;
		}

		// Nothing has been written yet, so refusing here leaves the set exactly
		// as it was. This is the whole value of the pre-pass and the reason it
		// is not merely a convenience.
		if (Failures > 0)
		{
			Summary = FString::Printf(
				TEXT("Aborted: %d of %d recipe(s) failed validation. Nothing was baked."),
				Failures, Batch.Num());

			UE_LOG(LogNoiseBakeManifest, Error, TEXT("[%s] %s"), *GetName(), *Summary);

			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
				TEXT("%s\n\nSee the Output Log for the reason on each, or use Validate All.\n\n")
				TEXT("Clear Validate All Before Baking to bake the ones that do pass."),
				*Summary)));

			MarkPackageDirty();
			return;
		}
	}

	// -- Bake pass ----------------------------------------------------------

	int32 Baked = 0;
	Failures = 0;

	{
		FScopedSlowTask Task((float)Batch.Num(), LOCTEXT("Baking", "Baking noise volumes"));
		Task.MakeDialog(/*bShowCancelButton*/ true);

		for (UNoiseBakeRecipeBase* Recipe : Batch)
		{
			if (Task.ShouldCancel())
			{
				UE_LOG(LogNoiseBakeManifest, Warning,
					TEXT("[%s] cancelled after %d bake(s)."), *GetName(), Baked);
				break;
			}

			Task.EnterProgressFrame(1.0f, FText::FromString(Recipe->GetName()));

			const double EntryStart = FPlatformTime::Seconds();

			// FNoiseVolumeBaker directly rather than Recipe->Bake(). The
			// recipe's own entry point opens a modal dialog on failure, which
			// across a batch means one dialog per broken recipe and a run that
			// cannot be left unattended.
			FString Error;
			const bool bOk = FNoiseVolumeBaker::Bake(Recipe, Error);

			FNoiseBakeBatchResult Entry;
			Entry.Recipe = Recipe->GetName();
			Entry.bSucceeded = bOk;
			Entry.Message = bOk ? Recipe->LastBakeStatus : Error;
			Entry.Seconds = (float)(FPlatformTime::Seconds() - EntryStart);

			Results.Add(Entry);

			if (bOk)
			{
				Baked++;
				Recipe->MarkPackageDirty();
			}
			else
			{
				Failures++;

				UE_LOG(LogNoiseBakeManifest, Error,
					TEXT("[%s] %s failed: %s"), *GetName(), *Recipe->GetName(), *Error);

				if (!bContinueOnFailure)
				{
					break;
				}
			}
		}
	}

	const double Duration = FPlatformTime::Seconds() - StartTime;

	Summary = FString::Printf(
		TEXT("Baked %d of %d recipe(s) in %.1fs. %d failed."),
		Baked, Batch.Num(), Duration, Failures);

	UE_LOG(LogNoiseBakeManifest, Log, TEXT("[%s] %s"), *GetName(), *Summary);

	MarkPackageDirty();
}

#undef LOCTEXT_NAMESPACE
