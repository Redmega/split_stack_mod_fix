#include "StackResizerSplitFix.h"

#include "StackSizeSyncSubsystem.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY( LogStackResizerSplitFix );

void FStackResizerSplitFixModule::StartupModule()
{
	UE_LOG( LogStackResizerSplitFix, Display, TEXT( "StackResizerSplitFix starting up" ) );

	FWorldDelegates::OnWorldInitializedActors.AddLambda(
		[]( const UWorld::FActorsInitializedParams& Params )
		{
			AStackSizeSyncSubsystem::RegisterWithWorld( Params.World );
		} );
}

IMPLEMENT_MODULE( FStackResizerSplitFixModule, StackResizerSplitFix )
