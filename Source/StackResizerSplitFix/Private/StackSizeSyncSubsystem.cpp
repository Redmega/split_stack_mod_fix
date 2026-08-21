#include "StackSizeSyncSubsystem.h"

#include "StackResizerSplitFix.h"

#include "Engine/World.h"
#include "ModLoading/PluginModuleLoader.h"
#include "Net/UnrealNetwork.h"
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

void AStackSizeSyncSubsystem::GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );

	DOREPLIFETIME( AStackSizeSyncSubsystem, AuthoritativeStackSizes );
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

	MaintenancePassesRemaining = MaintenancePassCount;
	ScheduleMaintenance();
}

int32 AStackSizeSyncSubsystem::GatherOnAuthority()
{
	TMap< UClass*, int32 > PreviousSizes;
	PreviousSizes.Reserve( AuthoritativeStackSizes.Num() );
	for( const FStackResizerSyncedStackSize& Entry : AuthoritativeStackSizes )
	{
		PreviousSizes.Add( Entry.ItemClass.Get(), Entry.StackSize );
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
		Entry.ItemClass = DescriptorClass;
		Entry.StackSize = StackSize;

		const int32* PreviousSize = PreviousSizes.Find( DescriptorClass );
		if( !PreviousSize || *PreviousSize != StackSize )
		{
			++ChangedCount;
		}
	}

	return ChangedCount;
}

void AStackSizeSyncSubsystem::OnRep_AuthoritativeStackSizes()
{
	const int32 Corrected = ApplyAuthoritativeSizes( TEXT( "OnRep" ) );

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
		if( !Entry.ItemClass )
		{
			// Class references arrive as NetGUIDs and may not have resolved yet; a later
			// maintenance pass will pick these up.
			continue;
		}

		const int32 LocalStackSize = UFGItemDescriptor::GetStackSize( Entry.ItemClass );
		if( LocalStackSize == Entry.StackSize )
		{
			continue;
		}

		if( SetCachedStackSize( Entry.ItemClass, Entry.StackSize ) )
		{
			++Corrected;

			UE_LOG( LogStackResizerSplitFix, Log, TEXT( "StackSizeSync[%s]: %s %d -> %d" ),
				Context, *GetNameSafe( Entry.ItemClass ), LocalStackSize, Entry.StackSize );
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
