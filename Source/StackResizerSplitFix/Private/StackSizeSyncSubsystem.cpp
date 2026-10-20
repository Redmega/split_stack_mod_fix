#include "StackSizeSyncSubsystem.h"

#include "StackResizerSplitFix.h"
#include "StackSizeSyncRCO.h"

#include "Engine/World.h"
#include "FGPlayerController.h"
#include "ModLoading/PluginModuleLoader.h"
#include "Subsystem/SubsystemActorManager.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"

namespace
{
	/**
	 * Sized from measurement - see the window-sizing table in docs/research-notes.md. The
	 * clobber has only ever been observed as a single event within ~2s of the sizes arriving,
	 * never once in ~95 later passes, so five seconds is already generous. The interval is 1s
	 * rather than 2s because it halves how long a client can sit in the wrong state.
	 */
	constexpr int32 MaintenancePassCount = 5;
	constexpr float MaintenanceIntervalSeconds = 1.0f;

	/**
	 * Entries per RPC. A batch has to stay under the receiver's 64KB cap on a reassembled bunch
	 * (net.MaxConstructedPartialBunchSizeBytes) even with long mod asset paths, and should use a
	 * small slice of the 256-bunch reliable buffer. 100 entries is ~10-30KB, ~10-35 bunches.
	 */
	constexpr int32 TransferBatchSize = 100;

	/** How long the client keeps looking for its RCO before concluding the server can't serve it. */
	constexpr int32 TransferStartAttempts = 60;
	constexpr float TransferRetryIntervalSeconds = 1.0f;

	/**
	 * mCachedStackSize is not public, but it is a UPROPERTY, so reflection reaches it without an
	 * Access Transformer. Worth it: transformers are injected into FactoryGame's own headers at
	 * build time, so adding one rebuilds that entire module rather than just this mod.
	 */
	FIntProperty* FindCachedStackSizeProperty()
	{
		static FIntProperty* CachedProperty = CastField< FIntProperty >(
			UFGItemDescriptor::StaticClass()->FindPropertyByName( TEXT( "mCachedStackSize" ) ) );
		return CachedProperty;
	}

	bool SetCachedStackSize( TSubclassOf< UFGItemDescriptor > ItemClass, int32 NewStackSize )
	{
		FIntProperty* Property = FindCachedStackSizeProperty();
		if( !Property || !ItemClass )
		{
			return false;
		}

		UObject* Descriptor = ItemClass->GetDefaultObject();
		if( !Descriptor )
		{
			return false;
		}

		Property->SetPropertyValue_InContainer( Descriptor, NewStackSize );
		return true;
	}
}

AStackSizeSyncSubsystem::AStackSizeSyncSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;

	// Inventory UI can be opened anywhere, so the sizes have to be correct everywhere.
	bAlwaysRelevant = true;
}

void AStackSizeSyncSubsystem::RegisterWithWorld( UWorld* World )
{
	if( !World || !FPluginModuleLoader::ShouldLoadModulesForWorld( World ) )
	{
		return;
	}

	// The main menu is its own world, in which the client is its own authority - without this it
	// would gather a full descriptor snapshot for nobody.
	if( FPluginModuleLoader::IsMainMenuWorld( World ) )
	{
		return;
	}

	if( USubsystemActorManager* SubsystemActorManager = World->GetSubsystem< USubsystemActorManager >() )
	{
		// Registering on both sides is deliberate: the server spawns the actor, and the client
		// needs the class registered for the manager to track the replicated copy.
		SubsystemActorManager->RegisterSubsystemActor( AStackSizeSyncSubsystem::StaticClass() );
	}
}

AStackSizeSyncSubsystem* AStackSizeSyncSubsystem::Get( UWorld* World )
{
	USubsystemActorManager* SubsystemActorManager = World ? World->GetSubsystem< USubsystemActorManager >() : nullptr;
	return SubsystemActorManager ? SubsystemActorManager->GetSubsystemActor< AStackSizeSyncSubsystem >() : nullptr;
}

void AStackSizeSyncSubsystem::BeginPlay()
{
	Super::BeginPlay();

	if( HasAuthority() )
	{
		GatherOnAuthority();

		UE_LOG( LogStackResizerSplitFix, Display,
			TEXT( "StackSizeSync: authority gathered %d descriptor stack sizes for replication" ),
			AuthoritativeStackSizes.Num() );
	}
	else
	{
		TransferStartAttemptsRemaining = TransferStartAttempts;
		TryStartTransfer();
	}

	MaintenancePassesRemaining = MaintenancePassCount;
	ScheduleMaintenance();
}

int32 AStackSizeSyncSubsystem::GatherOnAuthority()
{
	const TArray< FStackResizerSyncedStackSize > PreviousSnapshot = MoveTemp( AuthoritativeStackSizes );

	TMap< FSoftObjectPath, int32 > PreviousSizes;
	PreviousSizes.Reserve( PreviousSnapshot.Num() );
	for( const FStackResizerSyncedStackSize& Entry : PreviousSnapshot )
	{
		PreviousSizes.Add( Entry.ItemClass, Entry.StackSize );
	}

	// GetDerivedClasses only reports classes that are currently loaded, which is why this is
	// re-run by the maintenance passes rather than trusted as a one-shot snapshot.
	TArray< UClass* > DescriptorClasses;
	GetDerivedClasses( UFGItemDescriptor::StaticClass(), DescriptorClasses, true );

	AuthoritativeStackSizes.Reset();
	AuthoritativeStackSizes.Reserve( DescriptorClasses.Num() );

	int32 ChangedCount = 0;

	for( UClass* DescriptorClass : DescriptorClasses )
	{
		if( DescriptorClass->HasAnyClassFlags( CLASS_Abstract ) )
		{
			continue;
		}

		const int32 StackSize = UFGItemDescriptor::GetStackSize( DescriptorClass );
		if( StackSize <= 0 )
		{
			continue;
		}

		FStackResizerSyncedStackSize& Entry = AuthoritativeStackSizes.AddDefaulted_GetRef();
		Entry.ItemClass = FSoftClassPath( DescriptorClass );
		Entry.StackSize = StackSize;

		const int32* PreviousSize = PreviousSizes.Find( Entry.ItemClass );
		if( !PreviousSize || *PreviousSize != StackSize )
		{
			++ChangedCount;
		}
	}

	// Compared element-wise rather than inferred from ChangedCount, which misses removals and
	// reordering - either of which would shift the indices a client mid-transfer is relying on.
	if( AuthoritativeStackSizes != PreviousSnapshot )
	{
		++SnapshotGeneration;
	}

	return ChangedCount;
}

void AStackSizeSyncSubsystem::GetBatchOnAuthority( int32 StartIndex, int32& OutGeneration, int32& OutTotal,
	TArray< FStackResizerSyncedStackSize >& OutBatch ) const
{
	OutGeneration = SnapshotGeneration;
	OutTotal = AuthoritativeStackSizes.Num();

	const int32 First = FMath::Clamp( StartIndex, 0, OutTotal );
	const int32 Count = FMath::Min( TransferBatchSize, OutTotal - First );

	OutBatch.Reset( Count );
	OutBatch.Append( AuthoritativeStackSizes.GetData() + First, Count );
}

UStackSizeSyncRCO* AStackSizeSyncSubsystem::FindLocalRCO() const
{
	// GetFirstPlayerController is the local player on a client; there is no splitscreen to
	// disambiguate.
	UWorld* World = GetWorld();
	AFGPlayerController* PlayerController = World ? Cast< AFGPlayerController >( World->GetFirstPlayerController() ) : nullptr;
	return PlayerController ? PlayerController->GetRemoteCallObjectOfClass< UStackSizeSyncRCO >() : nullptr;
}

void AStackSizeSyncSubsystem::TryStartTransfer()
{
	if( FindLocalRCO() )
	{
		RequestBatch( 0 );
		return;
	}

	UWorld* World = GetWorld();
	if( !World )
	{
		return;
	}

	if( --TransferStartAttemptsRemaining <= 0 )
	{
		UE_LOG( LogStackResizerSplitFix, Error,
			TEXT( "StackSizeSync: no remote call object after %d attempts; stack sizes will not be synced. Is the server running this mod version?" ),
			TransferStartAttempts );
		return;
	}

	World->GetTimerManager().SetTimer( TransferTimerHandle, this,
		&AStackSizeSyncSubsystem::TryStartTransfer, TransferRetryIntervalSeconds, false );
}

void AStackSizeSyncSubsystem::RequestBatch( int32 StartIndex )
{
	if( UStackSizeSyncRCO* RCO = FindLocalRCO() )
	{
		RCO->ServerRequestStackSizes( StartIndex );
	}
}

void AStackSizeSyncSubsystem::ReceiveBatch( int32 Generation, int32 StartIndex, int32 Total,
	const TArray< FStackResizerSyncedStackSize >& Batch )
{
	if( StartIndex == 0 )
	{
		PendingStackSizes.Reset( Total );
		TransferGeneration = Generation;
	}
	else if( Generation != TransferGeneration || StartIndex != PendingStackSizes.Num() )
	{
		// The server's maintenance passes re-gathered mid-transfer, so the indices received so
		// far belong to a different snapshot.
		UE_LOG( LogStackResizerSplitFix, Log,
			TEXT( "StackSizeSync: snapshot changed during transfer (generation %d -> %d), restarting" ),
			TransferGeneration, Generation );
		RequestBatch( 0 );
		return;
	}

	PendingStackSizes.Append( Batch );

	// An empty batch short of Total would otherwise request the same index forever.
	if( PendingStackSizes.Num() < Total && Batch.Num() > 0 )
	{
		RequestBatch( PendingStackSizes.Num() );
		return;
	}

	AuthoritativeStackSizes = MoveTemp( PendingStackSizes );
	PendingStackSizes.Reset();

	int32 Unresolved = 0;
	for( FStackResizerSyncedStackSize& Entry : AuthoritativeStackSizes )
	{
		// A load rather than a find: the server only lists classes it has loaded, and the class
		// reference this replaced was loaded on arrival too.
		Entry.ResolvedClass = Entry.ItemClass.TryLoadClass< UFGItemDescriptor >();
		if( !Entry.ResolvedClass )
		{
			++Unresolved;
		}
	}

	if( Unresolved > 0 )
	{
		UE_LOG( LogStackResizerSplitFix, Warning,
			TEXT( "StackSizeSync: %d of %d descriptors from the server do not exist on this client; skipping them" ),
			Unresolved, AuthoritativeStackSizes.Num() );
	}

	const int32 Corrected = ApplyAuthoritativeSizes( TEXT( "transfer" ) );

	UE_LOG( LogStackResizerSplitFix, Display,
		TEXT( "StackSizeSync: received %d authoritative sizes, corrected %d on this client" ),
		AuthoritativeStackSizes.Num(), Corrected );

	// The sizes can arrive after BeginPlay already opened a window, so restart it from here -
	// the passes are only useful once there is something to compare against.
	MaintenancePassesRemaining = MaintenancePassCount;
	ScheduleMaintenance();
}

int32 AStackSizeSyncSubsystem::ApplyAuthoritativeSizes( const TCHAR* Context )
{
	if( !FindCachedStackSizeProperty() )
	{
		UE_LOG( LogStackResizerSplitFix, Error,
			TEXT( "StackSizeSync: could not resolve UFGItemDescriptor::mCachedStackSize; cannot apply sizes" ) );
		return 0;
	}

	int32 Corrected = 0;

	for( const FStackResizerSyncedStackSize& Entry : AuthoritativeStackSizes )
	{
		if( !Entry.ResolvedClass )
		{
			continue;
		}

		const int32 LocalStackSize = UFGItemDescriptor::GetStackSize( Entry.ResolvedClass );
		if( LocalStackSize == Entry.StackSize )
		{
			continue;
		}

		if( SetCachedStackSize( Entry.ResolvedClass, Entry.StackSize ) )
		{
			++Corrected;

			UE_LOG( LogStackResizerSplitFix, Log, TEXT( "StackSizeSync[%s]: %s %d -> %d" ),
				Context, *GetNameSafe( Entry.ResolvedClass ), LocalStackSize, Entry.StackSize );
		}
	}

	return Corrected;
}

void AStackSizeSyncSubsystem::ScheduleMaintenance()
{
	UWorld* World = GetWorld();
	if( !World )
	{
		return;
	}

	if( MaintenancePassesRemaining <= 0 )
	{
		World->GetTimerManager().ClearTimer( MaintenanceTimerHandle );

		UE_LOG( LogStackResizerSplitFix, Display,
			TEXT( "StackSizeSync: maintenance window closed after %d passes" ), MaintenancePassCount );
		return;
	}

	--MaintenancePassesRemaining;

	// Bound to the UObject, not a lambda capturing `this`: the lambda overload stores a bare
	// TFunction with no lifetime tracking, and leaving a session mid-window fired it on a
	// destroyed actor. Observed crash, not a hypothetical.
	World->GetTimerManager().SetTimer( MaintenanceTimerHandle, this,
		&AStackSizeSyncSubsystem::RunMaintenancePass, MaintenanceIntervalSeconds, false );
}

void AStackSizeSyncSubsystem::EndPlay( const EEndPlayReason::Type EndPlayReason )
{
	if( const UWorld* World = GetWorld() )
	{
		World->GetTimerManager().ClearTimer( MaintenanceTimerHandle );
		World->GetTimerManager().ClearTimer( TransferTimerHandle );
	}

	MaintenancePassesRemaining = 0;

	Super::EndPlay( EndPlayReason );
}

void AStackSizeSyncSubsystem::RunMaintenancePass()
{
	// Every pass logs, including the zero case: without a line, "nothing drifted" is
	// indistinguishable from "the timer never fired".
	const int32 PassNumber = MaintenancePassCount - MaintenancePassesRemaining;

	if( HasAuthority() )
	{
		const int32 Changed = GatherOnAuthority();

		UE_LOG( LogStackResizerSplitFix, Log,
			TEXT( "StackSizeSync[authority pass %d/%d]: %d entries new or changed (%d total)" ),
			PassNumber, MaintenancePassCount, Changed, AuthoritativeStackSizes.Num() );
	}
	else
	{
		const int32 Corrected = ApplyAuthoritativeSizes( TEXT( "maintenance" ) );

		UE_LOG( LogStackResizerSplitFix, Log,
			TEXT( "StackSizeSync[client pass %d/%d]: corrected %d of %d" ),
			PassNumber, MaintenancePassCount, Corrected, AuthoritativeStackSizes.Num() );

		if( Corrected > 0 )
		{
			UE_LOG( LogStackResizerSplitFix, Display,
				TEXT( "StackSizeSync: re-corrected %d descriptors that had drifted back" ), Corrected );
		}
	}

	ScheduleMaintenance();
}
