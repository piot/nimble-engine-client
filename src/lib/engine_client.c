/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "nimble-steps-serialize/out_serialize.h"
#include <nimble-engine-client/client.h>

void nimbleEngineClientInit(NimbleEngineClient* self, NimbleEngineClientSetup setup)
{
    if (!transmuteVmVersionIsEqual(&setup.predicted.version, &setup.authoritative.version)) {
        CLOG_C_ERROR(&setup.log, "not same transmuteVmVersion %d.%d.%d", setup.predicted.version.major,
                     setup.predicted.version.minor, setup.predicted.version.patch)
    }

    self->phase = NimbleEngineClientPhaseWaitingForInitialGameState;
    self->authoritative = setup.authoritative;
    self->predicted = setup.predicted;
    self->maxStepOctetSizeForSingleParticipant = setup.maximumSingleParticipantStepOctetCount;
    self->log = setup.log;
    self->maximumParticipantCount = setup.maximumParticipantCount;

    NimbleClientRealizeSettings realizeSetup;
    realizeSetup.memory = setup.memory;
    realizeSetup.blobMemory = setup.blobMemory;
    realizeSetup.transport = setup.transport;
    realizeSetup.maximumSingleParticipantStepOctetCount = setup.maximumSingleParticipantStepOctetCount;
    realizeSetup.maximumNumberOfParticipants = setup.maximumParticipantCount;
    realizeSetup.log = setup.log;
    nimbleClientRealizeInit(&self->nimbleClient, &realizeSetup);
}

void nimbleEngineClientRequestJoin(NimbleEngineClient* self, NimbleEngineClientGameJoinOptions options)
{
    NimbleSerializeGameJoinOptions joinOptions;

    for (size_t i = 0; i < options.playerCount; ++i) {
        joinOptions.players[i].localIndex = options.players[i].localIndex;
    }
    joinOptions.playerCount = options.playerCount;

    NimbleSerializeVersion applicationVersion = {self->authoritative.version.major, self->authoritative.version.minor,
                                                 self->authoritative.version.patch};
    joinOptions.applicationVersion = applicationVersion;
    nimbleClientRealizeJoinGame(&self->nimbleClient, joinOptions);
}

bool nimbleEngineClientMustAddPredictedInput(const NimbleEngineClient* self)
{
    bool allowedToAdd = nbsStepsAllowedToAdd(&self->nimbleClient.client.outSteps);
    if (!allowedToAdd) {
        return false;
    }

    // TODO: Add more logic here
    return true;
}

int nimbleEngineClientAddPredictedInput(NimbleEngineClient* self, const TransmuteInput* input)
{
    NimbleStepsOutSerializeLocalParticipants data;

    for (size_t i = 0; i < input->participantCount; ++i) {
        uint8_t participantId = input->participantInputs[i].participantId;
        if (participantId == 0) {
            CLOG_ERROR("participantID zero is reserved")
        }
        data.participants[i].participantIndex = participantId;
        data.participants[i].payload = input->participantInputs[i].input;
        data.participants[i].payloadCount = input->participantInputs[i].octetSize;
    }

    data.participantCount = input->participantCount;

    uint8_t buf[120];

    int octetCount = nbsStepsOutSerializeStep(&data, buf, 120);
    if (octetCount < 0) {
        CLOG_ERROR("seerAddPredictedSteps: could not serialize")
        return octetCount;
    }

    rectifyAddPredictedStepRaw(&self->rectify, buf, octetCount, self->nimbleClient.client.outSteps.expectedWriteId);

    return nbsStepsWrite(&self->nimbleClient.client.outSteps, self->nimbleClient.client.outSteps.expectedWriteId, buf,
                         octetCount);
}

static void tickInGame(NimbleEngineClient* self)
{
    uint8_t inputBuf[512];
    StepId authoritativeTickId;

    if (self->nimbleClient.client.authoritativeStepsFromServer.stepsCount == 0) {
        rectifyUpdate(&self->rectify);
        return;
    }

    int octetCount = nimbleClientReadStep(&self->nimbleClient.client, inputBuf, 512, &authoritativeTickId);
    if (octetCount < 0) {
        CLOG_C_ERROR(&self->log, " could not read");
    }

    int errorCode = rectifyAddAuthoritativeStepRaw(&self->rectify, inputBuf, octetCount, authoritativeTickId);
    if (errorCode < 0) {
        CLOG_C_ERROR(&self->log, "could not go on, can not add authoritative steps")
    }

    rectifyUpdate(&self->rectify);
}

static void joinGameState(NimbleEngineClient* self)
{
    self->phase = NimbleEngineClientPhaseInGame;

    RectifySetup rectifySetup;
    rectifySetup.allocator = self->nimbleClient.settings.memory;
    rectifySetup.maxStepOctetSizeForSingleParticipant = self->maxStepOctetSizeForSingleParticipant;
    rectifySetup.maxPlayerCount = self->maximumParticipantCount;
    rectifySetup.log = self->log;

    const NimbleClientGameState* joinedGameState = &self->nimbleClient.client.joinedGameState;
    TransmuteState joinedTransmuteState = {joinedGameState->gameState, joinedGameState->gameStateOctetCount};
    CLOG_C_DEBUG(&self->log, "Joined game state. octetCount: %zu step %04X", joinedGameState->gameStateOctetCount,
                 joinedGameState->stepId);

    rectifyInit(&self->rectify, self->authoritative, self->predicted, rectifySetup, joinedTransmuteState,
                joinedGameState->stepId);
}

void nimbleEngineClientUpdate(NimbleEngineClient* self)
{
    size_t targetFps;

    nimbleClientRealizeUpdate(&self->nimbleClient, monotonicTimeMsNow(), &targetFps);

    switch (self->phase) {
        case NimbleEngineClientPhaseWaitingForInitialGameState:
            switch (self->nimbleClient.state) {
                case NimbleClientRealizeStateInGame:
                    joinGameState(self);
                case NimbleClientRealizeStateInit:
                    break;
                case NimbleClientRealizeStateReInit:
                    break;
                case NimbleClientRealizeStateCleared:
                    break;
            }
            break;
        case NimbleEngineClientPhaseInGame:
            tickInGame(self);
            break;
    }
}

int nimbleEngineClientGetGameStates(NimbleEngineClient* self, NimbleGameState* authoritativeState,
                                    NimbleGameState* predictedState)
{
    authoritativeState->state = transmuteVmGetState(&self->rectify.authoritative.transmuteVm);
    authoritativeState->tickId = self->rectify.authoritative.stepId;

    predictedState->state = transmuteVmGetState(&self->rectify.predicted.transmuteVm);
    predictedState->tickId = self->rectify.predicted.stepId;

    return 0;
}

int nimbleEngineClientGetStats(const NimbleEngineClient* self, NimbleEngineClientStats* stats)
{
    stats->authoritativeBufferDeltaStat = self->nimbleClient.client.authoritativeBufferDeltaStat.avg;
    stats->stepCountInStepBufferOnServer = self->nimbleClient.client.stepCountInIncomingBufferOnServerStat.avg;

    return 0;
}
