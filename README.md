# Nimble Engine Client

Sending and receiving Steps (user input) to/from a Nimble Server as well as performing rollback and prediction.

Uses:

* [Nimble Client](https://github.com/piot/nimble-client-c) for handling the protocol part.
* [Rectify](https://github.com/piot/rectify-c) handles rollback and predicting using:
  * [Assent](https://github.com/piot/assent-c) that keeps track of the authoritative simulation and state.
  * [Seer](https://github.com/piot/seer-c) given an authoritative state and predicted Steps (input) it can predict the future.
  * [Transmute](https://github.com/piot/transmute-c). Simulation abstraction (used by both Assent and Seer). Can tick the simulation as well as get and set simulation state.


## Usage

### Initialize

Initializes and starts connecting to the server

```c
typedef struct NimbleEngineClientSetup {
    UdpTransportInOut transport;
    struct ImprintAllocator* memory;
    struct ImprintAllocatorWithFree* blobMemory;
    TransmuteVm authoritative;
    TransmuteVm predicted;
    size_t maximumSingleParticipantStepOctetCount;
    size_t maximumParticipantCount;
    NimbleSerializeVersion applicationVersion;
    Clog log;
} NimbleEngineClientSetup;

void nimbleEngineClientInit(NimbleEngineClient* self, NimbleEngineClientSetup setup);
```

### Joining Participants

Starts to join participants.

```c
typedef struct NimbleEngineClientPlayerJoinOptions {
    uint8_t localIndex;
} NimbleEngineClientPlayerJoinOptions;

typedef struct NimbleEngineClientGameJoinOptions {
    NimbleEngineClientPlayerJoinOptions players[8];
    size_t playerCount;
} NimbleEngineClientGameJoinOptions;

void nimbleEngineClientRequestJoin(NimbleEngineClient* self,
                                   NimbleEngineClientGameJoinOptions options);
```

### Update

Call this every tick.

```c
void nimbleEngineClientUpdate(NimbleEngineClient* self);
```

### Send Predicted Input

Only call `nimbleEngineClientAddPredictedInput()` if the `nimbleEngineClientMustAddPredictedInput()` returns true.

```c
bool nimbleEngineClientMustAddPredictedInput(const NimbleEngineClient* self);
int nimbleEngineClientAddPredictedInput(NimbleEngineClient* self, const TransmuteInput* input);
```

### Get Simulation States

Gets the two current simulation states, both the authoritative and the predicted one. The authoritative is mostly used for things that we do not want to present to the user
unless it is certain that it will happen, like getting a score in a game or similar. The predicted state is the one primarily used for presentation.

```c
typedef struct NimbleGameState {
    TransmuteState state;
    StepId tickId;
} NimbleGameState;

int nimbleEngineClientGetGameStates(NimbleEngineClient* self, NimbleGameState* authoritativeState,
                                    NimbleGameState* predictedState);
```
