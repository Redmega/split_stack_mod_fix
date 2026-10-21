#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "Resources/FGItemDescriptor.h"
#include "UObject/SoftObjectPath.h"
#include "StackSizeSyncSubsystem.generated.h"

class UStackSizeSyncRCO;

/** One item descriptor and the stack size the server considers correct for it. */
USTRUCT()
struct FStackResizerSyncedStackSize
{
	GENERATED_BODY()

	/**
	 * A path rather than a TSubclassOf: a class reference goes over the wire as a NetGUID export
	 * carrying the class's outer chain, measured at ~260 bytes per entry. A path is a fraction
	 * of that, which means fewer batches.
	 */
	UPROPERTY()
	FSoftClassPath ItemClass;

	UPROPERTY()
	int32 StackSize = INDEX_NONE;

	/**
	 * Client only: ItemClass loaded once when the transfer completes, so maintenance passes don't
	 * repeat the lookup. As a UPROPERTY it also keeps the class referenced, as the replicated
	 * TSubclassOf this replaced did. Never set on the server, so it is null on the wire regardless.
	 */
	UPROPERTY( NotReplicated )
	TSubclassOf< UFGItemDescriptor > ResolvedClass;

	bool operator==( const FStackResizerSyncedStackSize& Other ) const
	{
		return ItemClass == Other.ItemClass && StackSize == Other.StackSize;
	}
};

/**
 * Sends the server's effective stack size for every item descriptor to clients, and keeps the
 * client's descriptors pinned to those values.
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
 *
 * Transfer: the client pulls the snapshot in batches through UStackSizeSyncRCO, one batch in
 * flight at a time. It is deliberately not a replicated property: a replicated array goes out
 * whole in the actor's initial reliable bunch, and with 1827 descriptors that was 537 partial
 * bunches against a 256-bunch reliable buffer, so the server dropped the joining client
 * ("Outgoing reliable buffer overflow"). Batching keeps the cost per bunch flat however many
 * descriptors the mod list adds.
 */
UCLASS()
class AStackSizeSyncSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	AStackSizeSyncSubsystem();

	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type EndPlayReason ) override;

	/** Registers this subsystem with the world's SubsystemActorManager. Safe to call on either side. */
	static void RegisterWithWorld( UWorld* World );

	/** @return this world's instance, or null if it has not been spawned or replicated yet. */
	static AStackSizeSyncSubsystem* Get( UWorld* World );

	/** Server: copies out the batch of the current snapshot that starts at StartIndex. */
	void GetBatchOnAuthority( int32 StartIndex, int32& OutGeneration, int32& OutTotal,
		TArray< FStackResizerSyncedStackSize >& OutBatch ) const;

	/** Client: accepts one batch from the server and requests the next, or applies the whole snapshot. */
	void ReceiveBatch( int32 Generation, int32 StartIndex, int32 Total,
		const TArray< FStackResizerSyncedStackSize >& Batch );

private:
	/** @return number of entries added or changed since the last snapshot. */
	int32 GatherOnAuthority();

	/** @return number of descriptors corrected. */
	int32 ApplyAuthoritativeSizes( const TCHAR* Context );

	/** Client: the RCO only exists once the player controller has replicated, so this retries until it does. */
	void TryStartTransfer();
	void RequestBatch( int32 StartIndex );
	UStackSizeSyncRCO* FindLocalRCO() const;

	/**
	 * Both sides need a second look, for different reasons: the server because
	 * GetDerivedClasses only sees *loaded* classes, so descriptors loading later miss the first
	 * snapshot; the client because the subsystem that clobbers descriptors can run after us as
	 * easily as before us. Bounded, since this is a startup race.
	 */
	void ScheduleMaintenance();
	void RunMaintenancePass();

	/** Server: the snapshot being served. Client: the last complete snapshot received. */
	UPROPERTY()
	TArray< FStackResizerSyncedStackSize > AuthoritativeStackSizes;

	/** Client: batches received so far for the transfer in progress. */
	UPROPERTY()
	TArray< FStackResizerSyncedStackSize > PendingStackSizes;

	/**
	 * Bumped by the server whenever a re-gather changes the snapshot. Batch indices are only
	 * meaningful within one generation, so the client restarts a transfer that straddles a change.
	 */
	int32 SnapshotGeneration = 0;
	int32 TransferGeneration = INDEX_NONE;

	FTimerHandle TransferTimerHandle;
	int32 TransferStartAttemptsRemaining = 0;

	FTimerHandle MaintenanceTimerHandle;
	int32 MaintenancePassesRemaining = 0;
};
