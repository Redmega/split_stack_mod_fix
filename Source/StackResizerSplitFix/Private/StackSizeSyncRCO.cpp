#include "StackSizeSyncRCO.h"

#include "StackResizerSplitFix.h"

#include "Net/UnrealNetwork.h"

void UStackSizeSyncRCO::GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );

	DOREPLIFETIME( UStackSizeSyncRCO, DummyReplicatedField );
}

bool UStackSizeSyncRCO::ServerRequestStackSizes_Validate( int32 StartIndex )
{
	return StartIndex >= 0;
}

void UStackSizeSyncRCO::ServerRequestStackSizes_Implementation( int32 StartIndex )
{
	AStackSizeSyncSubsystem* Subsystem = AStackSizeSyncSubsystem::Get( GetWorld() );
	if( !Subsystem )
	{
		UE_LOG( LogStackResizerSplitFix, Warning,
			TEXT( "StackSizeSync: client requested sizes but the server has no sync subsystem" ) );
		return;
	}

	int32 Generation = 0;
	int32 Total = 0;
	TArray< FStackResizerSyncedStackSize > Batch;
	Subsystem->GetBatchOnAuthority( StartIndex, Generation, Total, Batch );

	ClientReceiveStackSizes( Generation, StartIndex, Total, Batch );
}

void UStackSizeSyncRCO::ClientReceiveStackSizes_Implementation( int32 Generation, int32 StartIndex, int32 Total,
	const TArray< FStackResizerSyncedStackSize >& Batch )
{
	if( AStackSizeSyncSubsystem* Subsystem = AStackSizeSyncSubsystem::Get( GetWorld() ) )
	{
		Subsystem->ReceiveBatch( Generation, StartIndex, Total, Batch );
	}
}
