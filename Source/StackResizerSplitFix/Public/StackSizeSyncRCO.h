#pragma once

#include "CoreMinimal.h"
#include "FGRemoteCallObject.h"
#include "StackSizeSyncSubsystem.h"
#include "StackSizeSyncRCO.generated.h"

/**
 * Per-player channel for AStackSizeSyncSubsystem's snapshot transfer. RPCs need an object owned
 * by the player's connection, which a subsystem actor is not; an RCO is, via its player
 * controller. The client pulls one batch at a time, so only one batch is ever in the reliable
 * buffer regardless of how many descriptors the mod list has.
 */
UCLASS( NotBlueprintable )
class UStackSizeSyncRCO : public UFGRemoteCallObject
{
	GENERATED_BODY()

public:
	UFUNCTION( Server, Reliable, WithValidation )
	void ServerRequestStackSizes( int32 StartIndex );

	UFUNCTION( Client, Reliable )
	void ClientReceiveStackSizes( int32 Generation, int32 StartIndex, int32 Total,
		const TArray< FStackResizerSyncedStackSize >& Batch );

	virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;

private:
	/** An RCO is not replicated to its client without at least one replicated property. */
	UPROPERTY( Replicated )
	int32 DummyReplicatedField = 0;
};
