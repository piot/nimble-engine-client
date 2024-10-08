/*----------------------------------------------------------------------------------------------------------
 *  Copyright (c) Peter Bjorklund. All rights reserved. https://github.com/piot/nimble-engine-client
 *  Licensed under the MIT License. See LICENSE in the project root for license information.
 *--------------------------------------------------------------------------------------------------------*/
#include <imprint/allocator.h>
#include <nimble-client/utils.h>
#include <nimble-engine-client/client.h>
#include <nimble-steps-serialize/out_serialize.h>
#include <tiny-libc/tiny_libc.h>

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
            CLOG_C_ERROR(&self->log, "could not read")
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

    if (addedStepCount > 0) {
        CLOG_C_VERBOSE(&self->log, "added %zu authoritative steps in one tick", addedStepCount)
    }

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
                 joinedGameState->stepId)

    rectifyInit(&self->rectify, self->rectifyCallbackObject, rectifySetup, joinedTransmuteState,
                joinedGameState->stepId);
    self->waitUntilAdjust = 0;
}

static size_t calculateOptimalPredictionCountThisTick(const NimbleEngineClient* self)
{
    size_t predictCount = 1U;

    //if (self->waitUntilAdjust > 0) {
        //return 1;
   // }

    StepId outOptimalPredictionTickId;
    size_t outDiffCount;
    bool worked = nimbleClientOptimalStepIdToSend(&self->nimbleClient.client, &outOptimalPredictionTickId,
                                                  &outDiffCount);
    if (!worked) {
        return 1;
    }

    int diffAheadOptimalTickCount = (int) self->nimbleClient.client.outSteps.expectedWriteId -
                                    (int) outOptimalPredictionTickId;
    if (diffAheadOptimalTickCount > 5) {
        predictCount = 0U;
    } else if (diffAheadOptimalTickCount < 0) {
        predictCount = 2U;
    }

    /*
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
    */

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

static void skipAheadIfNeeded(NimbleEngineClient* self)
{
    NimbleClient* client = &self->nimbleClient.client;

    StepId outOptimalPredictionTickId;
    size_t outDiffCount;
    bool worked = nimbleClientOptimalStepIdToSend(client, &outOptimalPredictionTickId, &outDiffCount);
    if (!worked) {
        return;
    }

    int numberOfTicksAhead = (int) outOptimalPredictionTickId - (int) client->outSteps.expectedWriteId;
    const int maxStepCountToWriteToBufferOverAReasonableTime = 16; // 2 writes each tick = 8 ticks = 128 ms
    if (numberOfTicksAhead < maxStepCountToWriteToBufferOverAReasonableTime) {
        return;
    }

    CLOG_EXECUTE(int serverBufferDiff = client->stepCountInIncomingBufferOnServerStat.avg;)

    // We are too much behind, just skip ahead the outgoing step buffer, so there is less to predict
    StepId newBaseStepId = client->outSteps.expectedWriteId + (StepId) numberOfTicksAhead;
    CLOG_C_NOTICE(
        &self->log,
        "SKIP AHEAD. Server says that we are %d behind. Client estimated skip ahead %d (%zu), from %08X to %08X",
        serverBufferDiff, numberOfTicksAhead, outDiffCount, client->outSteps.expectedWriteId, newBaseStepId)
    nbsStepsReInit(&client->outSteps, newBaseStepId);
    nbsStepsReInit(&self->rectify.predicted.predictedSteps, newBaseStepId);
    self->waitUntilAdjust = 0;
}

static int nimbleEngineClientAddPredictedInputHelper(NimbleEngineClient* self, const TransmuteInput* input)
{
    NimbleStepsOutSerializeLocalParticipants data;

    for (size_t i = 0; i < input->participantCount; ++i) {
        uint8_t participantId = input->participantInputs[i].participantId;
        if (participantId > 64) {
            CLOG_ERROR("too high participantID %hhu", participantId)
        }
        // Predicted is always in normal. Not allowed to insert forced steps
        data.participants[i].participantId = participantId;
        data.participants[i].payload = input->participantInputs[i].input;
        data.participants[i].stepType = NimbleSerializeStepTypeNormal;
        data.participants[i].payloadCount = input->participantInputs[i].octetSize;
    }

    data.participantCount = input->participantCount;

    uint8_t buf[120];

    ssize_t octetCount = nbsStepsOutSerializeCombinedStep(&data, buf, 120);
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

static int nimbleEngineClientTick(void* _self)
{
    NimbleEngineClient* self = (NimbleEngineClient*) _self;
    nimbleClientRealizeUpdate(&self->nimbleClient, monotonicTimeMsNow());

    const NimbleClient* client = &self->nimbleClient.client;

    if (client->outSteps.isInitialized && client->authoritativeStepsFromServer.isInitialized) {
        bool allowedToAdd = nbsStepsAllowedToAdd(&self->nimbleClient.client.outSteps);
        if (!allowedToAdd) {
            CLOG_C_WARN(&self->log, "out steps is full, not allowed to add steps %zu", self->nimbleClient.client.outSteps.stepsCount)
            return false;
        }

        self->shouldAddPredictedInput = true;

        skipAheadIfNeeded(self);
        size_t optimalPredictionCount = calculateOptimalPredictionCountThisTick(self);
        if (optimalPredictionCount && self->lastPredictedInput.participantCount > 0) {
            for (size_t i = 0; i < optimalPredictionCount; ++i) {
                if (self->rectify.predicted.predictedSteps.stepsCount >= 40U) {
                    break;
                }
                int err = nimbleEngineClientAddPredictedInputHelper(self, &self->lastPredictedInput);
                if (err < 0) {
                    return err;
                }
            }
        }
        if (optimalPredictionCount != 1) {
         //   self->waitUntilAdjust = 30;
        }
    }

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

/*
 NimbleSerializeVersion applicationVersion = {self->authoritative.version.major, self->authoritative.version.minor,
                                            self->authoritative.version.patch};
 *
 */
/// Initializes an nimble engine client using the setup
/// @param self nimble engine client
/// @param setup initial values
void nimbleEngineClientInit(NimbleEngineClient* self, NimbleEngineClientSetup setup)
{
    /*
    if (!transmuteVmVersionIsEqual(&setup.predicted.version, &setup.authoritative.version)) {
        CLOG_C_ERROR(&setup.log, "not same transmuteVmVersion %d.%d.%d", setup.predicted.version.major,
                     setup.predicted.version.minor, setup.predicted.version.patch)
    }
    */

    self->phase = NimbleEngineClientPhaseWaitingForInitialGameState;
    self->maxStepOctetSizeForSingleParticipant = setup.maximumSingleParticipantStepOctetCount;
    self->log = setup.log;
    self->maximumParticipantCount = setup.maximumParticipantCount;
    self->maxTicksFromAuthoritative = setup.maxTicksFromAuthoritative;
    self->ticksWithoutAuthoritativeSteps = 0;
    self->lastPredictedInput.participantCount = 0;
    self->shouldAddPredictedInput = true;

    self->lastPredictedInput.participantInputs = self->lastParticipantInputs;
    self->lastPredictedInput.participantCount = 0;

    self->inputBufferMaxSize = self->maxStepOctetSizeForSingleParticipant * self->maximumParticipantCount;
    self->inputBuffer = IMPRINT_ALLOC(setup.memory, self->inputBufferMaxSize, "Input Buffer");
    self->rectifyCallbackObject = setup.rectifyCallbackObject;

    statsHoldPositiveInit(&self->detectedGapInAuthoritativeSteps, 20U);
    statsHoldPositiveInit(&self->bigGapInAuthoritativeSteps, 20U);

    NimbleClientRealizeSettings realizeSetup;
    realizeSetup.memory = setup.memory;
    realizeSetup.blobMemory = setup.blobMemory;
    realizeSetup.transport = setup.transport;
    realizeSetup.maximumSingleParticipantStepOctetCount = setup.maximumSingleParticipantStepOctetCount;
    realizeSetup.maximumNumberOfParticipants = setup.maximumParticipantCount;
    realizeSetup.applicationVersion = setup.applicationVersion;
    realizeSetup.wantsDebugStreams = setup.wantsDebugStream;
    realizeSetup.log = setup.log;
    self->waitUntilAdjust = 0;
    const size_t targetDeltaTimeMs = 16U;
    timeTickInit(&self->timeTick, targetDeltaTimeMs, self, nimbleEngineClientTick, monotonicTimeMsNow(), self->log);
    timeTickSetQualityCheckEnabled(&self->timeTick, setup.useTimeTickQualityChecks);
    nimbleClientRealizeInit(&self->nimbleClient, &realizeSetup);
}

/// Asks to join the local participants to the game
/// @param self nimble engine client
/// @param options game join options
void nimbleEngineClientRequestJoin(NimbleEngineClient* self, NimbleEngineClientGameJoinOptions options)
{
    NimbleSerializeJoinGameRequest joinOptions;

    for (size_t i = 0; i < options.playerCount; ++i) {
        joinOptions.players[i].localIndex = options.players[i].localIndex;
    }

    joinOptions.playerCount = options.playerCount;
    joinOptions.partyAndSessionSecret = options.partyAndSessionSecret;
    joinOptions.joinGameType = options.type;
    // TODO: joinOptions.nonce

    nimbleClientRealizeJoinGame(&self->nimbleClient, joinOptions);
}

void nimbleEngineClientRequestDisconnect(NimbleEngineClient* client)
{
    nimbleClientRealizeQuitGame(&client->nimbleClient);
}

/// Checks if a predicted input must be added this tick
/// @param self nimble engine client
/// @return true if predicted input must be added
bool nimbleEngineClientMustAddPredictedInput(const NimbleEngineClient* self)
{
    return self->shouldAddPredictedInput;
}

/// Adds predicted input to the nimble engine client.
/// Only call this if nimbleEngineClientMustAddPredictedInput() returns true
/// @param self nimble engine client
/// @param input application specific transmute input
/// @return negative on error
int nimbleEngineClientAddPredictedInput(NimbleEngineClient* self, const TransmuteInput* input)
{
    uint8_t* p = self->inputBuffer;

    for (size_t i = 0; i < input->participantCount; ++i) {
        self->lastPredictedInput.participantInputs[i] = input->participantInputs[i];
        self->lastPredictedInput.participantInputs[i].inputType = TransmuteParticipantInputTypeNormal;
        tc_memcpy_octets(p, input->participantInputs[i].input, input->participantInputs[i].octetSize);
        self->lastPredictedInput.participantInputs[i].input = p;
        p += input->participantInputs[i].octetSize;
    }
    self->lastPredictedInput.participantCount = input->participantCount;

    return 0;
}

/// Update the nimble engine client
/// @param self nimble engine client
void nimbleEngineClientUpdate(NimbleEngineClient* self)
{
    timeTickUpdate(&self->timeTick, monotonicTimeMsNow());
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
