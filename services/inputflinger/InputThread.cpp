/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "InputThread.h"

#include <android-base/logging.h>
#include <com_android_input_flags.h>
#include <processgroup/processgroup.h>

namespace android {

namespace input_flags = com::android::input::flags;

namespace {

// Implementation of Thread from libutils.
class InputThreadImpl : public Thread {
public:
    explicit InputThreadImpl(std::function<void()> loop)
          : Thread(/* canCallJava */ true), mThreadLoop(loop) {}

    ~InputThreadImpl() {}

private:
    std::function<void()> mThreadLoop;

    bool threadLoop() override {
        mThreadLoop();
        return true;
    }
};

} // namespace

InputThread::InputThread(std::string name, std::function<void()> loop, std::function<void()> wake)
      : mName(name), mThreadWake(wake) {
    mThread = sp<InputThreadImpl>::make(loop);
    mThread->run(mName.c_str(), ANDROID_PRIORITY_URGENT_DISPLAY);
    if (input_flags::enable_input_policy_profile()) {
        if (!applyInputEventProfile()) {
            LOG(ERROR) << "Couldn't apply input policy profile for " << name;
        }
    }
}

InputThread::~InputThread() {
    mThread->requestExit();
    if (mThreadWake) {
        mThreadWake();
    }
    mThread->requestExitAndWait();
}

bool InputThread::isCallingThread() {
#if defined(__ANDROID__)
    return gettid() == mThread->getTid();
#else
    // Assume that the caller is doing everything correctly,
    // since thread information is not available on host
    return false;
#endif
}

bool InputThread::applyInputEventProfile() {
#if defined(__ANDROID__)
    return SetTaskProfiles(mThread->getTid(), {"InputPolicy"});
#else
    // Since thread information is not available and there's no benefit of
    // applying the task profile on host, return directly.
    return true;
#endif
}

} // namespace android