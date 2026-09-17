#pragma once

#include "../Engine/Engine.hpp"
#include "../../Utils/Memory.hpp"
#include "../../Utils/Logger/Logger.hpp"
#include "../Offsets.hpp"

class Features
{
public:
    bool QueueFirstJasonSandbox();
    bool HasQueuedJasonRequest() const;
    bool RequestFirstJasonSandbox();
    bool ConsumeQueuedJasonRequestOnGameThread();
    void TickAIOnly();
};

extern Features* func;

// F3 requests one counselor bot. Current proven mode expects F1 AI Jason first.
bool QueueCounselorBotRequest();
