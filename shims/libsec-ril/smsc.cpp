/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "libsec-ril-shim"

#include <dlfcn.h>
#include <string.h>

#include <mutex>
#include <string>
#include <unordered_set>

#include <log/log.h>
#include <telephony/ril.h>

namespace {

constexpr char kRealRil[] = "/vendor/lib64/libsec-ril-impl.so";

using RilInit = const RIL_RadioFunctions* (*)(const RIL_Env*, int, char**);

void* real_handle;
const RIL_Env* ril_env;
RIL_Env shim_ril_env;
RIL_RadioFunctions shim_functions;
RIL_RequestFunc real_on_request;

std::mutex smsc_mutex;
std::unordered_set<RIL_Token> smsc_requests;

RilInit RealInit(const char* name) {
    if (real_handle == nullptr) {
        real_handle = dlopen(kRealRil, RTLD_NOW);
        if (real_handle == nullptr) {
            RLOGE("dlopen %s: %s", kRealRil, dlerror());
            return nullptr;
        }
    }

    auto init = reinterpret_cast<RilInit>(dlsym(real_handle, name));
    if (init == nullptr) {
        RLOGE("dlsym %s: %s", name, dlerror());
    }
    return init;
}

// Samsung's RIL answers GET_SMSC_ADDRESS in the 3GPP TS 27.007 +CSCA form "<digits>",<tosca>,
// while ril.h expects the address itself, with a '+' for an international number. Telephony hands
// the reply back as the SMSC of SEND_SMS and to the IMS service, so it has to be an address.
bool ToRilAddress(const std::string& reply, std::string* address) {
    if (reply.size() < 4 || reply[0] != '"') return false;

    const size_t quote = reply.find('"', 1);
    if (quote == std::string::npos || quote + 2 >= reply.size() || reply[quote + 1] != ',') {
        return false;
    }

    const std::string digits = reply.substr(1, quote - 1);
    if (digits.find_first_not_of("0123456789") != std::string::npos) return false;

    const std::string tosca = reply.substr(quote + 2);
    if (tosca.size() > 3 || tosca.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }

    int toa = 0;
    for (char c : tosca) {
        toa = toa * 10 + (c - '0');
    }
    if (toa > 0xff) return false;

    // Type of number, 3GPP TS 23.040 9.1.2.5
    const bool international = (toa & 0x70) == 0x10;
    *address = international && !digits.empty() ? "+" + digits : digits;
    return true;
}

void OnRequestComplete(RIL_Token t, RIL_Errno e, void* response, size_t response_len) {
    bool smsc;
    {
        std::lock_guard<std::mutex> lock(smsc_mutex);
        smsc = smsc_requests.erase(t) != 0;
    }

    if (smsc && e == RIL_E_SUCCESS && response != nullptr) {
        const auto reply = static_cast<const char*>(response);
        const std::string original(reply, strnlen(reply, response_len));
        std::string address;
        if (ToRilAddress(original, &address)) {
            RLOGI("GET_SMSC_ADDRESS %s -> %s", original.c_str(), address.c_str());
            ril_env->OnRequestComplete(t, e, address.data(), address.size() + 1);
            return;
        }
        RLOGW("GET_SMSC_ADDRESS reply %s left as is", original.c_str());
    }

    ril_env->OnRequestComplete(t, e, response, response_len);
}

void OnRequest(int request, void* data, size_t data_len, RIL_Token t) {
    {
        std::lock_guard<std::mutex> lock(smsc_mutex);
        if (request == RIL_REQUEST_GET_SMSC_ADDRESS) {
            smsc_requests.insert(t);
        } else {
            smsc_requests.erase(t);
        }
    }

    real_on_request(request, data, data_len, t);
}

}  // namespace

extern "C" const RIL_RadioFunctions* RIL_Init(const RIL_Env* env, int argc, char** argv) {
    const RilInit init = RealInit("RIL_Init");
    if (init == nullptr) return nullptr;

    ril_env = env;
    shim_ril_env = *env;
    shim_ril_env.OnRequestComplete = OnRequestComplete;

    const RIL_RadioFunctions* functions = init(&shim_ril_env, argc, argv);
    if (functions == nullptr || functions->onRequest == nullptr) return functions;

    shim_functions = *functions;
    real_on_request = functions->onRequest;
    shim_functions.onRequest = OnRequest;
    return &shim_functions;
}

extern "C" const RIL_RadioFunctions* RIL_SAP_Init(const RIL_Env* env, int argc, char** argv) {
    const RilInit init = RealInit("RIL_SAP_Init");
    return init != nullptr ? init(env, argc, argv) : nullptr;
}
