/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "ECOService"

#include "eco/ECOService.h"

#include <binder/BinderService.h>
#include <cutils/atomic.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <ctime>
#include <string>

#include "eco/ECODebug.h"

namespace android {
namespace media {
namespace eco {

// ----------------------------------------------------------------------------
// Logging support -- this is for debugging only
// Use "adb shell dumpsys media.ECOService -v 1" to change it.
volatile int32_t gLogLevel = 0;

#define LOG1(...) ALOGD_IF(gLogLevel >= 1, __VA_ARGS__);
#define LOG2(...) ALOGD_IF(gLogLevel >= 2, __VA_ARGS__);

static void setLogLevel(int level) {
    android_atomic_write(level, &gLogLevel);
}

ECOService::ECOService() : BnECOService() {
    ALOGD("ECOService created");
    setLogLevel(10);
}

/*virtual*/ ::android::binder::Status ECOService::obtainSession(
        int32_t width, int32_t height, bool isCameraRecording,
        ::android::sp<::android::media::eco::IECOSession>* _aidl_return) {
    ALOGI("ECOService::obtainSession w: %d, h: %d, isCameraRecording: %d", width, height,
          isCameraRecording);

    if (width <= 0) {
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Width can not be <= 0");
    }

    if (height <= 0) {
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Height can not be <= 0");
    }

    if (!isCameraRecording) {
        return STATUS_ERROR(ERROR_ILLEGAL_ARGUMENT, "Only support camera recording");
    }

    SessionConfig newCfg(width, height, isCameraRecording);

    ALOGD("session count before is %zu", mSessionConfigToSessionMap.size());

    Mutex::Autolock lock(mServiceLock);
    bool foundSession = false;
    // Instead of looking up the map directly, take the chance to scan the map and evict all the
    // invalid sessions.
    SanitizeSession([&](MapIterType iter) {
        if (iter->first == newCfg) {
            sp<IECOSession> session = iter->second.promote();
            foundSession = true;
            *_aidl_return = session;
        }
    });

    if (foundSession) {
        return binder::Status::ok();
    }

    // Create a new session and add it to the record.
    *_aidl_return = ECOSession::createECOSession(width, height, isCameraRecording);
    if (*_aidl_return == nullptr) {
        ALOGE("ECOService failed to create ECOSession w: %d, h: %d, isCameraRecording: %d", width,
              height, isCameraRecording);
        return STATUS_ERROR(ERROR_UNSUPPORTED, "Failed to create eco session");
    }
    // Insert the new session into the map.
    mSessionConfigToSessionMap[newCfg] = *_aidl_return;
    ALOGD("session count after is %zu", mSessionConfigToSessionMap.size());

    return binder::Status::ok();
}

/*virtual*/ ::android::binder::Status ECOService::getNumOfSessions(int32_t* _aidl_return) {
    Mutex::Autolock lock(mServiceLock);
    SanitizeSession(std::function<void(MapIterType it)>());  // empty callback
    *_aidl_return = mSessionConfigToSessionMap.size();
    return binder::Status::ok();
}

/*virtual*/ ::android::binder::Status ECOService::getSessions(
        ::std::vector<::android::sp<::android::IBinder>>* _aidl_return) {
    // Clear all the entries in the vector.
    _aidl_return->clear();

    Mutex::Autolock lock(mServiceLock);
    SanitizeSession([&](MapIterType iter) {
        sp<IECOSession> session = iter->second.promote();
        _aidl_return->push_back(IInterface::asBinder(session));
    });
    return binder::Status::ok();
}

inline bool isEmptySession(const android::wp<IECOSession>& entry) {
    sp<IECOSession> session = entry.promote();
    return session == nullptr;
}

void ECOService::SanitizeSession(
        const std::function<void(std::unordered_map<SessionConfig, wp<IECOSession>,
                                                    SessionConfigHash>::iterator it)>& callback) {
    for (auto it = mSessionConfigToSessionMap.begin(), end = mSessionConfigToSessionMap.end();
         it != end;) {
        if (isEmptySession(it->second)) {
            it = mSessionConfigToSessionMap.erase(it);
        } else {
            if (callback != nullptr) {
                callback(it);
            };
            it++;
        }
    }
}

/*virtual*/ void ECOService::binderDied(const wp<IBinder>& /*who*/) {}

}  // namespace eco
}  // namespace media
}  // namespace android