/*
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

#include "Power.h"

#include <android-base/logging.h>

namespace aidl::android::hardware::power::impl::tuna {

using ndk::ScopedAStatus;

namespace {

ScopedAStatus unsupported() {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

bool modeSupported(Mode type) {
    return type == Mode::INTERACTIVE || type == Mode::LAUNCH;
}

bool boostSupported(Boost type) {
    return type == Boost::INTERACTION;
}

}  // namespace

Power::Power(power_module_t* module) : mModule(module) {
    if (mModule != nullptr && mModule->init != nullptr) {
        mModule->init(mModule);
    }
}

ScopedAStatus Power::setMode(Mode type, bool enabled) {
    if (mModule == nullptr || !modeSupported(type)) {
        return ScopedAStatus::ok();
    }
    switch (type) {
        case Mode::INTERACTIVE:
            if (mModule->setInteractive != nullptr) {
                mModule->setInteractive(mModule, enabled ? 1 : 0);
            }
            break;
        case Mode::LAUNCH:
            if (mModule->powerHint != nullptr) {
                int32_t launching = enabled ? 1 : 0;
                mModule->powerHint(mModule, POWER_HINT_LAUNCH, &launching);
            }
            break;
        default:
            break;
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Power::isModeSupported(Mode type, bool* _aidl_return) {
    *_aidl_return = mModule != nullptr && modeSupported(type);
    return ScopedAStatus::ok();
}

ScopedAStatus Power::setBoost(Boost type, int32_t durationMs) {
    if (mModule == nullptr || !boostSupported(type)) {
        return ScopedAStatus::ok();
    }
    if (mModule->powerHint != nullptr) {
        mModule->powerHint(mModule, POWER_HINT_INTERACTION, &durationMs);
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Power::isBoostSupported(Boost type, bool* _aidl_return) {
    *_aidl_return = mModule != nullptr && boostSupported(type);
    return ScopedAStatus::ok();
}

ScopedAStatus Power::createHintSession(int32_t, int32_t, const std::vector<int32_t>&, int64_t,
                                       std::shared_ptr<IPowerHintSession>*) {
    return unsupported();
}

ScopedAStatus Power::createHintSessionWithConfig(int32_t, int32_t, const std::vector<int32_t>&,
                                                 int64_t, SessionTag, SessionConfig*,
                                                 std::shared_ptr<IPowerHintSession>*) {
    return unsupported();
}

ScopedAStatus Power::getHintSessionPreferredRate(int64_t*) {
    return unsupported();
}

ScopedAStatus Power::getSessionChannel(int32_t, int32_t, ChannelConfig*) {
    return unsupported();
}

ScopedAStatus Power::closeSessionChannel(int32_t, int32_t) {
    return unsupported();
}

ScopedAStatus Power::getSupportInfo(SupportInfo* _aidl_return) {
    SupportInfo info;
    info.usesSessions = false;
    info.modes = (1LL << static_cast<int32_t>(Mode::INTERACTIVE)) |
                 (1LL << static_cast<int32_t>(Mode::LAUNCH));
    info.boosts = 1LL << static_cast<int32_t>(Boost::INTERACTION);
    info.sessionHints = 0;
    info.sessionModes = 0;
    info.sessionTags = 0;
    info.compositionData.isSupported = false;
    info.compositionData.disableGpuFences = false;
    info.compositionData.maxBatchSize = 1;
    info.compositionData.alwaysBatch = false;
    info.headroom.isCpuSupported = false;
    info.headroom.isGpuSupported = false;
    info.headroom.cpuMinIntervalMillis = 0;
    info.headroom.gpuMinIntervalMillis = 0;
    *_aidl_return = info;
    return ScopedAStatus::ok();
}

ScopedAStatus Power::getCpuHeadroom(const CpuHeadroomParams&, CpuHeadroomResult*) {
    return unsupported();
}

ScopedAStatus Power::getGpuHeadroom(const GpuHeadroomParams&, GpuHeadroomResult*) {
    return unsupported();
}

ScopedAStatus Power::sendCompositionData(const std::vector<CompositionData>&) {
    return unsupported();
}

ScopedAStatus Power::sendCompositionUpdate(const CompositionUpdate&) {
    return unsupported();
}

}  // namespace aidl::android::hardware::power::impl::tuna
