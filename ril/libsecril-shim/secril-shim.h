#ifndef __SECRIL_SHIM_H__
#define __SECRIL_SHIM_H__

#define LOG_TAG "secril-shim"
#define RIL_SHLIB

#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <binder/Parcel.h>
#include <cutils/compiler.h>
#include <cutils/properties.h>
#include <sys/cdefs.h>
#include <telephony/ril.h>
#include <utils/Log.h>

#define RIL_LIB_PATH "/vendor/lib/libsec-ril.so"

/* libsec-ril speaks RIL v6, whose RIL_RadioState folded the SIM, RUIM and
 * NV readiness into the radio state (values from the AOSP 4.3 ril.h).
 * Android 11's RIL_RadioState keeps only OFF (0), UNAVAILABLE (1) and
 * ON (10); the shim decodes the states in between itself. */
enum LegacyRadioState {
	LEGACY_RADIO_STATE_SIM_NOT_READY = 2,
	LEGACY_RADIO_STATE_SIM_LOCKED_OR_ABSENT = 3,
	LEGACY_RADIO_STATE_SIM_READY = 4,
	LEGACY_RADIO_STATE_RUIM_NOT_READY = 5,
	LEGACY_RADIO_STATE_RUIM_READY = 6,
	LEGACY_RADIO_STATE_RUIM_LOCKED_OR_ABSENT = 7,
	LEGACY_RADIO_STATE_NV_NOT_READY = 8,
	LEGACY_RADIO_STATE_NV_READY = 9,
};

enum variant_type {
	VARIANT_INIT,
	VARIANT_MAGURO,
	VARIANT_TORO,
	VARIANT_TOROPLUS,
	VARIANT_UNKNOWN
};

extern "C" const char * requestToString(int request);

/* TODO: Do we really need to redefine these? They aren't in a header... */
typedef struct {
	int requestNumber;
	void (*dispatchFunction) (android::Parcel &p, struct RequestInfo *pRI);
	int(*responseFunction) (android::Parcel &p, void *response, size_t responselen);
} CommandInfo;

typedef struct RequestInfo {
	int32_t token;
	CommandInfo *pCI;
	struct RequestInfo *p_next;
	char cancelled;
	char local;
	RIL_SOCKET_ID socket_id;
	int wasAckSent;
} RequestInfo;

#endif /* __SECRIL_SHIM_H__ */
