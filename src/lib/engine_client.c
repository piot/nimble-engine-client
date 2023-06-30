/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved.
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------*/
#include "nimble-steps-serialize/out_serialize.h"
#include <nimble-engine-client/client.h>

static void tickIncomingAuthoritativeSteps(NimbleEngineClient* self)
{
    uint8_t inputBuf[512];
    StepId authoritativeTickId;
    size_t addedStepCount = 0;

    for (size_t i = 0; i < 30; ++i) {
        if (self->nimbleClient.client.authoritativeStepsFromServer.stepsCount == 0) {
            break;
        }

        int octetCount = nimbleClientReadStep(&self->nimbleClient.client, inputBuf, 512, &authoritativeTickId);
        if (octetCount < 0) {
            CLOG_C_ERROR(&self->log, "could not read");
        }

        int errorCode = rectifyAddAuthoritativeStepRaw(&self->rectify, inputBuf, (size_t) octetCount,
                                                       authoritativeTickId);
        if (errorCode < 0) {
            CLOG_C_ERROR(&self->log, "could not go on, can not add authoritative steps")
        }
        addedStepCount++;
    }

    if (addedStepCount > 0) {
        self->ticksWithoutAuthoritativeSteps = 0;
    } else {
        self->ticksWithoutAuthoritativeSteps++;
    }

    CLOG_C_VERBOSE(&self->log, "added %zu authoritative steps in one tick", addedStepCount)

    bool hasGapInAuthoritativeSteps = self->ticksWithoutAuthoritativeSteps >= 2;
    statsHoldPositiveAdd(&self->detectedGapInAuthoritativeSteps, hasGapInAuthoritativeSteps);

    bool hasBigGapInAuthoritativeSteps = self->ticksWithoutAuthoritativeSteps >= 5;
    statsHoldPositiveAdd(&self->bigGapInAuthoritativeSteps, hasBigGapInAuthoritativeSteps);

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
    CLOG_C_DEBUG(&self->log, "Joined game state. octetCount: %zu step %08X", joinedGameState->gameStateOctetCount,
                 joinedGameState->stepId);

    rectifyInit(&self->rectify, self->authoritative, self->predicted, rectifySetup, joinedTransmuteState,
                joinedGameState->stepId);
    self->waitUntilAdjust = 50;
}

static size_t calculateOptimalPredictionCountThisTick(const NimbleEngineClient* self)
{
    size_t predictCount = 1U;

    if (self->waitUntilAdjust > 0) {
        return 1;
    }

    bool hasLatencyStat = self->nimbleClient.client.latencyMsStat.avgIsSet;
    int diffOptimalTickCount = 0;

    if (hasLatencyStat) {
        size_t averageLatency = (size_t) self->nimbleClient.client.latencyMsStat.avg;
        size_t optimalPredictionTickCount = (averageLatency / self->authoritative.constantTickDurationMs) + 1U;
        diffOptimalTickCount = (int) self->nimbleClient.client.outSteps.stepsCount - (int) optimalPredictionTickCount;
    }

    bool hasBufferDeltaAverage = self->nimbleClient.client.authoritativeBufferDeltaStat.avgIsSet;
    if (hasBufferDeltaAverage) {
        int bufferDeltaAverage = self->nimbleClient.client.authoritativeBufferDeltaStat.avg;
        if (bufferDeltaAverage < 2) {
            if (diffOptimalTickCount < 0) {
                // CLOG_C_INFO(&self->log, "too low buffer %d. add double predict count", bufferDeltaAverage)
                predictCount = 2;
            } else if (diffOptimalTickCount > 3) {
                // CLOG_C_INFO(&self->log, "we have gone too far %d, stopping simulation", bufferDeltaAverage)
                predictCount = 0;
            }
        }
        if (bufferDeltaAverage > 5) {
            // CLOG_C_INFO(&self->log, "too much buffer %d. stopping simulation", bufferDeltaAverage)
            predictCount = 0;
        }
    }

    // Check if it will fill up predicted buffer
    const size_t maximumNumberOfPredictedStepsInBuffer = 30U;
    int availableUntilFull = (int) maximumNumberOfPredictedStepsInBuffer -
                             (int) self->rectify.predicted.predictedSteps.stepsCount;
    if (availableUntilFull <= 0) {
        return 0;
    }
    if (predictCount > (size_t) availableUntilFull) {
        predictCount = (size_t) availableUntilFull;
    }

    return predictCount;
}

static int nimbleEngineClientTick(void* _self)
{
    NimbleEngineClient* self = (NimbleEngineClient*) _self;
    nimbleClientRealizeUpdate(&self->nimbleClient, monotonicTimeMsNow());

    bool allowedToAdd = nbsStepsAllowedToAdd(&self->nimbleClient.client.outSteps);
    if (!allowedToAdd) {
        CLOG_C_WARN(&self->log, "was not allowed to add steps")
        return false;
    }

    size_t optimalPredictionCount = calculateOptimalPredictionCountThisTick(self);
    self->shouldAddPredictedInput = optimalPredictionCount > 0;

    if (self->waitUntilAdjust > 0) {
        self->waitUntilAdjust--;
    }

    switch (self->phase) {
        case NimbleEngineClientPhaseWaitingForInitialGameState:
            switch (self->nimbleClient.state) {
                case NimbleClientRealizeStateSynced:
                    receivedGameState(self);
                    break;
                case NimbleClientRealizeStateInit:
                case NimbleClientRealizeStateReInit:
                case NimbleClientRealizeStateCleared:
                case NimbleClientRealizeStateDisconnected:
                    break;
            }
            break;
        case NimbleEngineClientPhaseSynced:
            tickSynced(self);
            break;
    }

    return 0;
}

/// Initializes an nimble engine client using the setup
/// @param self nimble engine client
/// @param setup initial values
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
    self->shouldAddPredictedInput = false;
    self->ticksWithoutAuthoritativeSteps = 0;

    statsHoldPositiveInit(&self->detectedGapInAuthoritativeSteps, 20U);
    statsHoldPositiveInit(&self->bigGapInAuthoritativeSteps, 20U);

    NimbleClientRealizeSettings realizeSetup;
    realizeSetup.memory = setup.memory;
    realizeSetup.blobMemory = setup.blobMemory;
    realizeSetup.transport = setup.transport;
    realizeSetup.maximumSingleParticipantStepOctetCount = setup.maximumSingleParticipantStepOctetCount;
    realizeSetup.maximumNumberOfParticipants = setup.maximumParticipantCount;
    realizeSetup.applicationVersion = setup.applicationVersion;
    realizeSetup.log = setup.log;
    self->waitUntilAdjust = 0;
    const size_t targetDeltaTimeMs = 16U;
    timeTickInit(&self->timeTick, targetDeltaTimeMs, self, nimbleEngineClientTick, monotonicTimeMsNow(), self->log);
    nimbleClientRealizeInit(&self->nimbleClient, &realizeSetup);
}

/// Asks to join the local participants to the game
/// @param self nimble engine client
/// @param options game join options
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

/// Checks if a predicted input must be added this tick
/// @param self nimble engine client
/// @return true if predicted input must be added
bool nimbleEngineClientMustAddPredictedInput(const NimbleEngineClient* self)
{
    return self->shouldAddPredictedInput;
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

    ssize_t octetCount = nbsStepsOutSerializeStep(&data, buf, 120);
    if (octetCount < 0) {
        CLOG_ERROR("seerAddPredictedSteps: could not serialize")
        // return (int) octetCount;
    }

    // CLOG_C_VERBOSE(&self->log, "PredictedCount: %zu outStepCount: %zu",
    // self->rectify.predicted.predictedSteps.stepsCount, self->nimbleClient.client.outSteps.stepsCount)

    rectifyAddPredictedStep(&self->rectify, input, self->nimbleClient.client.outSteps.expectedWriteId);

    return nbsStepsWrite(&self->nimbleClient.client.outSteps, self->nimbleClient.client.outSteps.expectedWriteId, buf,
                         (size_t) octetCount);
}

/// Adds predicted input to the nimble engine client.
/// Only call this if nimbleEngineClientMustAddPredictedInput() returns true
/// @param self nimble engine client
/// @param input application specific transmute input
/// @return negative on error
int nimbleEngineClientAddPredictedInput(NimbleEngineClient* self, const TransmuteInput* input)
{
    self->shouldAddPredictedInput = false;

    bool hasBufferDeltaAverage = self->nimbleClient.client.authoritativeBufferDeltaStat.avgIsSet;
    if (hasBufferDeltaAverage && self->waitUntilAdjust == 0) {
        int bufferDeltaAverage = self->nimbleClient.client.authoritativeBufferDeltaStat.avg;
        if (bufferDeltaAverage < -10) {
            // We are too much behind, just skip ahead the outgoing step buffer, so there is less to predict
            StepId newBaseStepId = self->nimbleClient.client.outSteps.expectedWriteId + 10;
            CLOG_C_NOTICE(&self->log, "SKIP AHEAD. Server says that we are %d behind, so we skip from %08X to %08X",
                          bufferDeltaAverage, self->nimbleClient.client.outSteps.expectedWriteId, newBaseStepId)
            nbsStepsReInit(&self->nimbleClient.client.outSteps, newBaseStepId);
            nbsStepsReInit(&self->rectify.predicted.predictedSteps, newBaseStepId);
            self->waitUntilAdjust = 30;
        }
    }
    size_t optimalPredictionCount = calculateOptimalPredictionCountThisTick(self);
    if (optimalPredictionCount)
        for (size_t i = 0; i < optimalPredictionCount; ++i) {
            if (self->rectify.predicted.predictedSteps.stepsCount >= 40U) {
                break;
            }
            int err = nimbleEngineClientAddPredictedInputHelper(self, input);
            if (err < 0) {
                return err;
            }
        }
    if (optimalPredictionCount != 1) {
        self->waitUntilAdjust = 30;
    }
    return 0;
}

/// Update the nimble engine client
/// @param self nimble engine client
void nimbleEngineClientUpdate(NimbleEngineClient* self)
{
    timeTickUpdate(&self->timeTick, monotonicTimeMsNow());
}

/// Gets the current authoritative and predicted states
/// @param self nimble engine client
/// @param authoritativeState application specific complete game state
/// @param predictedState current predicted state
/// @return negative error
int nimbleEngineClientGetGameStates(const NimbleEngineClient* self, NimbleGameState* authoritativeState,
                                    NimbleGameState* predictedState)
{
    authoritativeState->state = transmuteVmGetState(&self->rectify.authoritative.transmuteVm);
    authoritativeState->tickId = self->rectify.authoritative.stepId;

    predictedState->state = transmuteVmGetState(&self->rectify.predicted.transmuteVm);
    predictedState->tickId = self->rectify.predicted.stepId;

    return 0;
}

/// Gets statistics about the connection and operation of the nimble engine client.
/// @param self nimble engine client
/// @param[out] stats returns the client stats
/// @return negative on error
int nimbleEngineClientGetStats(const NimbleEngineClient* self, NimbleEngineClientStats* stats)
{
    stats->authoritativeBufferDeltaStat = self->nimbleClient.client.authoritativeBufferDeltaStat.avg;
    stats->stepCountInStepBufferOnServer = self->nimbleClient.client.stepCountInIncomingBufferOnServerStat.avg;

    return 0;
}
