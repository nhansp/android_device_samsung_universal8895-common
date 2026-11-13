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

void* sRealHandle;
const RIL_Env* sEnv;
RIL_Env sShimEnv;
RIL_RadioFunctions sShimFunctions;
RIL_RequestFunc sRealOnRequest;

std::mutex sLock;
std::unordered_set<RIL_Token> sSmscRequests;

RilInit realInit(const char* name) {
    if (sRealHandle == nullptr) {
        sRealHandle = dlopen(kRealRil, RTLD_NOW);
        if (sRealHandle == nullptr) {
            RLOGE("dlopen %s: %s", kRealRil, dlerror());
            return nullptr;
        }
    }

    auto init = reinterpret_cast<RilInit>(dlsym(sRealHandle, name));
    if (init == nullptr) {
        RLOGE("dlsym %s: %s", name, dlerror());
    }
    return init;
}

// Samsung's RIL answers GET_SMSC_ADDRESS in the 3GPP TS 27.007 +CSCA form "<digits>",<tosca>,
// while ril.h expects the address itself, with a '+' for an international number. Telephony hands
// the reply back as the SMSC of SEND_SMS and to the IMS service, so it has to be an address.
bool toRilAddress(const std::string& reply, std::string* address) {
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

void onRequestComplete(RIL_Token t, RIL_Errno e, void* response, size_t responselen) {
    bool smsc;
    {
        std::lock_guard<std::mutex> lock(sLock);
        smsc = sSmscRequests.erase(t) != 0;
    }

    if (smsc && e == RIL_E_SUCCESS && response != nullptr) {
        const auto reply = static_cast<const char*>(response);
        const std::string original(reply, strnlen(reply, responselen));
        std::string address;
        if (toRilAddress(original, &address)) {
            RLOGI("GET_SMSC_ADDRESS %s -> %s", original.c_str(), address.c_str());
            sEnv->OnRequestComplete(t, e, address.data(), address.size() + 1);
            return;
        }
        RLOGW("GET_SMSC_ADDRESS reply %s left as is", original.c_str());
    }

    sEnv->OnRequestComplete(t, e, response, responselen);
}

void onRequest(int request, void* data, size_t datalen, RIL_Token t) {
    {
        std::lock_guard<std::mutex> lock(sLock);
        if (request == RIL_REQUEST_GET_SMSC_ADDRESS) {
            sSmscRequests.insert(t);
        } else {
            sSmscRequests.erase(t);
        }
    }

    sRealOnRequest(request, data, datalen, t);
}

}  // namespace

extern "C" const RIL_RadioFunctions* RIL_Init(const RIL_Env* env, int argc, char** argv) {
    const RilInit init = realInit("RIL_Init");
    if (init == nullptr) return nullptr;

    sEnv = env;
    sShimEnv = *env;
    sShimEnv.OnRequestComplete = onRequestComplete;

    const RIL_RadioFunctions* functions = init(&sShimEnv, argc, argv);
    if (functions == nullptr || functions->onRequest == nullptr) return functions;

    sShimFunctions = *functions;
    sRealOnRequest = functions->onRequest;
    sShimFunctions.onRequest = onRequest;
    return &sShimFunctions;
}

extern "C" const RIL_RadioFunctions* RIL_SAP_Init(const RIL_Env* env, int argc, char** argv) {
    const RilInit init = realInit("RIL_SAP_Init");
    return init != nullptr ? init(env, argc, argv) : nullptr;
}
