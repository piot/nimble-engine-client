/*----------------------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved. https://github.com/piot/nimble-engine-client
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------------------*/
#ifndef NIMBLE_ENGINE_CLIENT_H
#define NIMBLE_ENGINE_CLIENT_H

#include <nimble-client/network_realizer.h>
#include <rectify/rectify.h>
#include <stats/hold_positive.h>
#include <time-tick/time_tick.h>
#include <transmute/transmute.h>

typedef struct NimbleGameState {
    TransmuteState state;
    StepId tickId;
} NimbleGameState;

typedef struct NimbleEngineClientSetup {
    DatagramTransport transport;
    RectifyCallbackObject rectifyCallbackObject;
    struct ImprintAllocator* memory;
    struct ImprintAllocatorWithFree* blobMemory;
    size_t maximumSingleParticipantStepOctetCount;
    size_t maximumParticipantCount;
    NimbleSerializeVersion applicationVersion;
    size_t maxTicksFromAuthoritative;
    bool wantsDebugStream;
    Clog log;
} NimbleEngineClientSetup;

typedef enum NimbleEngineClientPhase {
    NimbleEngineClientPhaseWaitingForInitialGameState,
    NimbleEngineClientPhaseSynced,
} NimbleEngineClientPhase;

typedef struct NimbleEngineClient {
    Rectify rectify;
    RectifyCallbackObject rectifyCallbackObject;
    NimbleClientRealize nimbleClient;
    NimbleEngineClientPhase phase;
    size_t maxStepOctetSizeForSingleParticipant;
    size_t maximumParticipantCount;
    size_t maxTicksFromAuthoritative;
    int waitUntilAdjust;
    TimeTick timeTick;
    Clog log;
    bool shouldAddPredictedInput;
    size_t ticksWithoutAuthoritativeSteps;
    StatsHoldPositive detectedGapInAuthoritativeSteps;
    StatsHoldPositive bigGapInAuthoritativeSteps;
    TransmuteInput lastPredictedInput;
    TransmuteParticipantInput lastParticipantInputs[32];
    uint8_t* inputBuffer;
    size_t inputBufferMaxSize;
} NimbleEngineClient;

typedef struct NimbleEngineClientPlayerJoinOptions {
    uint8_t localIndex;
} NimbleEngineClientPlayerJoinOptions;

typedef struct NimbleEngineClientGameJoinOptions {
    NimbleEngineClientPlayerJoinOptions players[8];
    size_t playerCount;
    NimbleSerializeJoinGameType type;
    NimbleSerializeParticipantConnectionSecret secret;
    NimbleSerializeParticipantId participantId;
} NimbleEngineClientGameJoinOptions;

typedef struct NimbleEngineClientStats {
    int authoritativeBufferDeltaStat;
    int stepCountInStepBufferOnServer;
} NimbleEngineClientStats;

void nimbleEngineClientInit(NimbleEngineClient* self, NimbleEngineClientSetup setup);
void nimbleEngineClientRequestJoin(NimbleEngineClient* self, NimbleEngineClientGameJoinOptions options);
void nimbleEngineClientRequestDisconnect(NimbleEngineClient* self);
void nimbleEngineClientUpdate(NimbleEngineClient* self);
bool nimbleEngineClientMustAddPredictedInput(const NimbleEngineClient* self);
int nimbleEngineClientAddPredictedInput(NimbleEngineClient* self, const TransmuteInput* input);
int nimbleEngineClientGetStats(const NimbleEngineClient* self, NimbleEngineClientStats* stats);

#endif
