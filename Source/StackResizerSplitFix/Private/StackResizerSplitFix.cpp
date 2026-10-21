#include "StackResizerSplitFix.h"

#include "StackSizeSyncRCO.h"
#include "StackSizeSyncSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Registry/RemoteCallObjectRegistry.h"

DEFINE_LOG_CATEGORY( LogStackResizerSplitFix );

void FStackResizerSplitFixModule::StartupModule()
{
	UE_LOG( LogStackResizerSplitFix, Display, TEXT( "StackResizerSplitFix starting up" ) );

	// The registry hands its classes to each game mode as it initialises, which happens after
	// world initialisation - so registering here is early enough. Repeat calls are de-duplicated.
	FWorldDelegates::OnPostWorldInitialization.AddLambda(
		[]( UWorld* World, const UWorld::InitializationValues )
		{
			UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			if( URemoteCallObjectRegistry* Registry = GameInstance ? GameInstance->GetSubsystem< URemoteCallObjectRegistry >() : nullptr )
			{
				Registry->RegisterRemoteCallObject( UStackSizeSyncRCO::StaticClass() );
			}
		} );

	FWorldDelegates::OnWorldInitializedActors.AddLambda(
		[]( const UWorld::FActorsInitializedParams& Params )
		{
			AStackSizeSyncSubsystem::RegisterWithWorld( Params.World );
		} );
}

IMPLEMENT_MODULE( FStackResizerSplitFixModule, StackResizerSplitFix )
