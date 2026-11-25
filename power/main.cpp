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
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <hardware/hardware.h>

#include <cstdlib>

using aidl::android::hardware::power::impl::tuna::Power;

int main() {
    const hw_module_t* hwModule = nullptr;
    int rc = hw_get_module(POWER_HARDWARE_MODULE_ID, &hwModule);
    if (rc != 0 || hwModule == nullptr) {
        LOG(ERROR) << "power module not found: " << rc;
        return EXIT_FAILURE;
    }

    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<Power> power = ndk::SharedRefBase::make<Power>(
            reinterpret_cast<power_module_t*>(const_cast<hw_module_t*>(hwModule)));

    const std::string instance = std::string() + Power::descriptor + "/default";
    binder_status_t status = AServiceManager_addService(power->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK);

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}
