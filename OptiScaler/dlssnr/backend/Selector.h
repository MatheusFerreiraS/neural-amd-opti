#pragma once
#include "Kind.h"

namespace DlssNr::Backend
{
Kind RequestedKind();
Kind ActiveKindFromConfig();
// True while lmxxf (with LmxxfWired()) or mochizuki is active. Mutual exclusion vs graphics tracker.
bool SubmissionHooksWanted();
} // namespace DlssNr::Backend
