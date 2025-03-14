// Copyright 2019 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "starboard/media.h"

#include "starboard/common/log.h"

#if SB_API_VERSION < 15

void SbMediaSetAudioWriteDuration(int64_t duration) {
  // The stub implementation assumes no further action is needed from the
  // platform to be compatible with limits >= 0.5 second.
  SB_DCHECK(duration >= 500'000)
      << "Limiting audio to less than 0.5 seconds is unexpected.";
}

#endif  // SB_API_VERSION < 15
