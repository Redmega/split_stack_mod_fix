#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "Resources/FGItemDescriptor.h"
#include "StackSizeSyncSubsystem.generated.h"

/** One item descriptor and the stack size the server considers correct for it. */
USTRUCT()
struct FStackResizerSyncedStackSize
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf< UFGItemDescriptor > ItemClass;

	UPROPERTY()
	int32 StackSize = INDEX_NONE;
};

/**
 * Replicates the server's effective stack size for every item descriptor to clients, and
 * keeps the client's descriptors pinned to those values.
 *
 * Why this exists: Stack Resizer applies its sizes from a mod subsystem that writes each
 * UFGItemDescriptor CDO's cached stack size. That is a purely local write with no replication,
 * so each side has to do it independently - and on the client it is fragile. SML subsystem
 * registration order is not deterministic between launches, and when another mod's subsystem
 * initialises *after* Stack Resizer's, the client's descriptors get reset to their vanilla
 * sizes while the server keeps the resized ones. The client then refuses to split any stack
 * larger than the vanilla size, because UFGInventoryComponent::CanSplitStackAtIdx compares
 * against the descriptor the client can see. See docs/research-notes.md for the log evidence.
 *
 * Rather than fight for a position in that ordering, this treats the server as the authority
 * (it is the side that is reliably correct) and repairs the client afterwards. That is immune
 * both to registration order and to *which* mod does the clobbering. Confirmed working
 * 2026-08-21: a client that came up with 290 vanilla descriptors was corrected on join and
 * every previously-failing split then succeeded.
 */
UCLASS()
class AStackSizeSyncSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	AStackSizeSyncSubsystem();

	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type EndPlayReason ) override;
	virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;

	/** Registers this subsystem with the world's SubsystemActorManager. Safe to call on either side. */
	static void RegisterWithWorld( UWorld* World );

protected:
	UFUNCTION()
	void OnRep_AuthoritativeStackSizes();

private:
	/** @return number of entries added or changed since the last snapshot. */
	int32 GatherOnAuthority();

	/** @return number of descriptors corrected. */
	int32 ApplyAuthoritativeSizes( const TCHAR* Context );

	/**
	 * Both sides need a second look, for different reasons: the server because
	 * GetDerivedClasses only sees *loaded* classes, so descriptors loading later miss the first
	 * snapshot; the client because the subsystem that clobbers descriptors can run after us as
	 * easily as before us. Bounded, since this is a startup race.
	 */
	void ScheduleMaintenance();
	void RunMaintenancePass();

	UPROPERTY( ReplicatedUsing = OnRep_AuthoritativeStackSizes )
	TArray< FStackResizerSyncedStackSize > AuthoritativeStackSizes;

	FTimerHandle MaintenanceTimerHandle;
	int32 MaintenancePassesRemaining = 0;
};
