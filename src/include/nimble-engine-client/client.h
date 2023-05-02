/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#ifndef NIMBLE_ENGINE_CLIENT_H
#define NIMBLE_ENGINE_CLIENT_H

#include <nimble-client/network_realizer.h>
#include <rectify/rectify.h>
#include <transmute/transmute.h>

typedef struct NimbleGameState {
    TransmuteState state;
    StepId tickId;
} NimbleGameState;

typedef struct NimbleEngineClientSetup {
    UdpTransportInOut transport;
    struct ImprintAllocator* memory;
    struct ImprintAllocatorWithFree* blobMemory;
    TransmuteVm authoritative;
    TransmuteVm predicted;
    size_t maximumSingleParticipantStepOctetCount;
    size_t maximumParticipantCount;
    NimbleSerializeVersion applicationVersion;
    size_t maxTicksFromAuthoritative;
    Clog log;
} NimbleEngineClientSetup;

typedef enum NimbleEngineClientPhase {
    NimbleEngineClientPhaseWaitingForInitialGameState,
    NimbleEngineClientPhaseSynced,
} NimbleEngineClientPhase;

typedef struct NimbleEngineClient {
    Rectify rectify;
    NimbleClientRealize nimbleClient;
    NimbleEngineClientPhase phase;
    TransmuteVm authoritative;
    TransmuteVm predicted;
    size_t maxStepOctetSizeForSingleParticipant;
    size_t maximumParticipantCount;
    size_t maxTicksFromAuthoritative;
    Clog log;
} NimbleEngineClient;

typedef struct NimbleEngineClientPlayerJoinOptions {
    uint8_t localIndex;
} NimbleEngineClientPlayerJoinOptions;

typedef struct NimbleEngineClientGameJoinOptions {
    NimbleEngineClientPlayerJoinOptions players[8];
    size_t playerCount;
} NimbleEngineClientGameJoinOptions;

typedef struct NimbleEngineClientStats {
    int authoritativeBufferDeltaStat;
    int stepCountInStepBufferOnServer;
} NimbleEngineClientStats;

void nimbleEngineClientInit(NimbleEngineClient* self, NimbleEngineClientSetup setup);
void nimbleEngineClientRequestJoin(NimbleEngineClient* self, NimbleEngineClientGameJoinOptions options);
void nimbleEngineClientUpdate(NimbleEngineClient* self);
bool nimbleEngineClientMustAddPredictedInput(const NimbleEngineClient* self);
int nimbleEngineClientAddPredictedInput(NimbleEngineClient* self, const TransmuteInput* input);

int nimbleEngineClientGetGameStates(NimbleEngineClient* self, NimbleGameState* authoritativeState,
                                    NimbleGameState* predictedState);
int nimbleEngineClientGetStats(const NimbleEngineClient* self, NimbleEngineClientStats* stats);

#endif
