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
    self->maxTicksFromAuthoritative = setup.maxTicksFromAuthoritative;

    NimbleClientRealizeSettings realizeSetup;
    realizeSetup.memory = setup.memory;
    realizeSetup.blobMemory = setup.blobMemory;
    realizeSetup.transport = setup.transport;
    realizeSetup.maximumSingleParticipantStepOctetCount = setup.maximumSingleParticipantStepOctetCount;
    realizeSetup.maximumNumberOfParticipants = setup.maximumParticipantCount;
    realizeSetup.applicationVersion = setup.applicationVersion;
    realizeSetup.log = setup.log;
    self->waitUntilAdjust = 0;
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

static int calculateOptimalPredictionCount(const NimbleEngineClient* self)
{
    size_t predictCount = 1U;

    if (self->waitUntilAdjust > 0) {
        return 1;
    }

    int optimalPredictionTickCount = 0U;
    bool hasLatencyStat = self->nimbleClient.client.latencyMsStat.avgIsSet;
    int diffOptimalTickCount = 0U;

    if (hasLatencyStat) {
        int averageLatency = self->nimbleClient.client.latencyMsStat.avg;
        optimalPredictionTickCount = (averageLatency / self->authoritative.constantTickDurationMs) + 1;
        diffOptimalTickCount = self->nimbleClient.client.outSteps.stepsCount - optimalPredictionTickCount;
    }

    bool hasBufferDeltaAverage = self->nimbleClient.client.authoritativeBufferDeltaStat.avgIsSet;
    if (hasBufferDeltaAverage) {
        int bufferDeltaAverage = self->nimbleClient.client.authoritativeBufferDeltaStat.avg;
        if (bufferDeltaAverage < 2) {
            if (diffOptimalTickCount < 0) {
                CLOG_C_INFO(&self->log, "too low buffer %d. add double predict count", bufferDeltaAverage)
                predictCount = 2;
            } else if (diffOptimalTickCount > 3) {
                CLOG_C_INFO(&self->log, "we have gone too far %d, stopping simulation", bufferDeltaAverage)
                predictCount = 0;
            }
        }
        if (bufferDeltaAverage > 5) {
            CLOG_C_INFO(&self->log, "too much buffer %d. stopping simulation", bufferDeltaAverage)
            predictCount = 0;
        }
    }

    //predictCount = 1;

    return predictCount;
}

bool nimbleEngineClientMustAddPredictedInput(const NimbleEngineClient* self)
{
    bool allowedToAdd = nbsStepsAllowedToAdd(&self->nimbleClient.client.outSteps);
    if (!allowedToAdd) {
        CLOG_C_WARN(&self->log, "was not allowed to add steps")
        return false;
    }

    int optimalPredictionCount = calculateOptimalPredictionCount(self);
    return optimalPredictionCount > 0;


    bool rectifyWantsPredicted = rectifyMustAddPredictedStepThisTick(&self->rectify);
    if (!rectifyWantsPredicted) {
        return false;
    }

    // TODO: Add more logic here

    return true;
}

static int nimbleEngineClientAddPredictedInputHelper(NimbleEngineClient* self, const TransmuteInput* input)
{
    NimbleStepsOutSerializeLocalParticipants data;

    for (size_t i = 0; i < input->participantCount; ++i) {
        uint8_t participantId = input->participantInputs[i].participantId;
        if (participantId == 0) {
            CLOG_ERROR("participantID zero is reserved")
        }
        if (participantId > 32) {
            CLOG_ERROR("too high participantID")
        }
        data.participants[i].participantId = participantId;
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

    // CLOG_C_VERBOSE(&self->log, "PredictedCount: %zu outStepCount: %zu",
    // self->rectify.predicted.predictedSteps.stepsCount, self->nimbleClient.client.outSteps.stepsCount)

    rectifyAddPredictedStep(&self->rectify, input, self->nimbleClient.client.outSteps.expectedWriteId);

    return nbsStepsWrite(&self->nimbleClient.client.outSteps, self->nimbleClient.client.outSteps.expectedWriteId, buf,
                         octetCount);
}

int nimbleEngineClientAddPredictedInput(NimbleEngineClient* self, const TransmuteInput* input)
{
    int optimalPredictionCount = calculateOptimalPredictionCount(self);

    for (size_t i = 0U; i < optimalPredictionCount; ++i) {
        int err = nimbleEngineClientAddPredictedInputHelper(self, input);
        if (err < 0) {
            return err;
        }
    }
    if (optimalPredictionCount != 1) {
        self->waitUntilAdjust = 10;
    }
    return 0;
}

static void tickIncomingAuthoritativeSteps(NimbleEngineClient* self)
{
    uint8_t inputBuf[512];
    StepId authoritativeTickId;
    size_t addedStepCount = 0;

    for (size_t i = 0; i < 30; ++i) {
        if (self->nimbleClient.client.authoritativeStepsFromServer.stepsCount == 0) {
            break;
            return;
        }

        int octetCount = nimbleClientReadStep(&self->nimbleClient.client, inputBuf, 512, &authoritativeTickId);
        if (octetCount < 0) {
            CLOG_C_ERROR(&self->log, "could not read");
        }

        int errorCode = rectifyAddAuthoritativeStepRaw(&self->rectify, inputBuf, octetCount, authoritativeTickId);
        if (errorCode < 0) {
            CLOG_C_ERROR(&self->log, "could not go on, can not add authoritative steps")
        }
        addedStepCount++;
    }

    CLOG_C_VERBOSE(&self->log, "added %d authoritative steps in one tick", addedStepCount)

    rectifyUpdate(&self->rectify);
}

static void tickSynced(NimbleEngineClient* self)
{
    tickIncomingAuthoritativeSteps(self);
}

static void receivedGameState(NimbleEngineClient* self)
{
    self->phase = NimbleEngineClientPhaseSynced;

    RectifySetup rectifySetup;
    rectifySetup.allocator = self->nimbleClient.settings.memory;
    rectifySetup.maxStepOctetSizeForSingleParticipant = self->maxStepOctetSizeForSingleParticipant;
    rectifySetup.maxPlayerCount = self->maximumParticipantCount;
    rectifySetup.log = self->log;
    rectifySetup.maxTicksFromAuthoritative = self->maxTicksFromAuthoritative;

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

    if (self->waitUntilAdjust > 0) {
        self->waitUntilAdjust--;
    }

    switch (self->phase) {
        case NimbleEngineClientPhaseWaitingForInitialGameState:
            switch (self->nimbleClient.state) {
                case NimbleClientRealizeStateSynced:
                    receivedGameState(self);
                case NimbleClientRealizeStateInit:
                    break;
                case NimbleClientRealizeStateReInit:
                    break;
                case NimbleClientRealizeStateCleared:
                    break;
            }
            break;
        case NimbleEngineClientPhaseSynced:
            tickSynced(self);
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
