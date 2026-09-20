// SPDX-License-Identifier: LGPL-2.1-or-later
// Stable command-record heads retained by deferred queue submissions.
#pragma once

#include <deque>

namespace owned_graphics {

template <typename Record>
using StableSubmissionStorage = std::deque<Record>;

} // namespace owned_graphics
