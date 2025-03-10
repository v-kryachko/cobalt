// Copyright 2023 The Cobalt Authors. All Rights Reserved.
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

#ifndef STARBOARD_SHARED_WIN32_POSIX_EMU_INCLUDE_POSIX_FORCE_INCLUDE_H_
#define STARBOARD_SHARED_WIN32_POSIX_EMU_INCLUDE_POSIX_FORCE_INCLUDE_H_

#if defined(STARBOARD)

// MSVC deprecated strdup() in favor of _strdup()
#define strdup _strdup

#define free sb_free

#include <time.h>  // For struct timespec

// https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/time.h.html
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3

#ifdef __cplusplus
extern "C" {
#endif

// https://pubs.opengroup.org/onlinepubs/9699919799/functions/clock_gettime.html
int clock_gettime(clockid_t clock_id, struct timespec* tp);

// https://pubs.opengroup.org/onlinepubs/000095399/functions/gmtime_r.html
struct tm* gmtime_r(const time_t* timer, struct tm* result);

int posix_memalign(void** res, size_t alignment, size_t size);

// https://pubs.opengroup.org/onlinepubs/9699919799/functions/strcasecmp.html
int strcasecmp(const char* s1, const char* s2);
int strncasecmp(const char* s1, const char* s2, size_t n);

#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // defined(STARBOARD)

#endif  // STARBOARD_SHARED_WIN32_POSIX_EMU_INCLUDE_POSIX_FORCE_INCLUDE_H_
