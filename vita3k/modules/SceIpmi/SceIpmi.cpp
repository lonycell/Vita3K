// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <module/module.h>

#include "modules/module_parent.h"

#include <cpu/functions.h>
#include <kernel/state.h>
#include <util/tracy.h>

#include <cstring>

static uint32_t ipmi_caller_lr(EmuEnvState &emuenv, SceUID thread_id) {
    const auto thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return 0;
    return read_lr(*thread->cpu);
}

TRACY_MODULE_NAME(SceIpmi);

EXPORT(int, _ZN4IPMI6Client10disconnectEv) {
    return UNIMPLEMENTED();
}

EXPORT(int, _ZN4IPMI6Client11getUserDataEv) {
    return UNIMPLEMENTED();
}

struct BufferInfo {
    Ptr<void> pBuffer;
    SceSize bufferSize;
    SceSize bufferWrittenSize; // size written by method
};

EXPORT(int, _ZN4IPMI6Client12tryGetResultEjPiPvPmm, Ptr<void> thisptr, uint32_t method_id, Ptr<int> result, Ptr<void> out, Ptr<uint32_t> out_size, uint32_t a6) {
    LOG_INFO("[IPMI-DIAG] tryGetResult this={} method={}", thisptr.address(), log_hex(method_id));
    return UNIMPLEMENTED();
}

EXPORT(int, _ZN4IPMI6Client12tryGetResultEjjPiPNS_10BufferInfoEj, Ptr<void> thisptr, uint32_t method_id, uint32_t a3, Ptr<int> result, Ptr<BufferInfo> out, uint32_t a6) {
    LOG_INFO("[IPMI-DIAG] tryGetResult(BufferInfo) this={} method={}", thisptr.address(), log_hex(method_id));
    return UNIMPLEMENTED();
}

EXPORT(int, _ZN4IPMI6Client13pollEventFlagEjjjPj, Ptr<void> thisptr, uint32_t a2, uint32_t a3, uint32_t a4, Ptr<uint32_t> out_pattern) {
    LOG_INFO("[IPMI-DIAG] pollEventFlag this={} a2={} a3={} a4={}", thisptr.address(), a2, a3, a4);
    return UNIMPLEMENTED();
}

EXPORT(int, _ZN4IPMI6Client13waitEventFlagEjjjPjS1_, Ptr<void> thisptr, uint32_t a2, uint32_t a3, uint32_t a4, Ptr<uint32_t> a5, Ptr<uint32_t> a6) {
    LOG_INFO("[IPMI-DIAG] waitEventFlag this={} a2={} a3={} a4={}", thisptr.address(), a2, a3, a4);
    return UNIMPLEMENTED();
}

EXPORT(int, _ZN4IPMI6Client16invokeSyncMethodEjPKNS_8DataInfoEjPiPNS_10BufferInfoEj, Ptr<void> thisptr, uint32_t method_id, Ptr<void> in_data, uint32_t in_num, Ptr<int> result, Ptr<BufferInfo> out, uint32_t out_num) {
    LOG_INFO("[IPMI-DIAG] invokeSyncMethod(DataInfo) this={} method={} inNum={} outNum={} caller_lr={}", thisptr.address(), log_hex(method_id), in_num, out_num, log_hex(ipmi_caller_lr(emuenv, thread_id)));
    // No real NP service daemon exists. Return a clean, zero-filled response so the
    // caller parses a well-formed "empty / signed-out" result instead of reading
    // uninitialised stack (which made the NP worker fail with 0x80551D01).
    if (out && out_num) {
        BufferInfo *bufs = out.get(emuenv.mem);
        for (uint32_t i = 0; i < out_num; ++i) {
            if (bufs[i].pBuffer && bufs[i].bufferSize) {
                memset(bufs[i].pBuffer.get(emuenv.mem), 0, bufs[i].bufferSize);
                bufs[i].bufferWrittenSize = bufs[i].bufferSize;
            }
        }
    }
    if (result)
        *result.get(emuenv.mem) = 0;
    return 0;
}

EXPORT(int, _ZN4IPMI6Client16invokeSyncMethodEjPKvjPiPvPjj, Ptr<void> thisptr, uint32_t method_id, Ptr<void> in_data, uint32_t in_size, Ptr<int> result, Ptr<void> out, Ptr<uint32_t> out_size, uint32_t unk) {
    LOG_INFO("[IPMI-DIAG] invokeSyncMethod this={} method={} inSize={} outSizePtr={}", thisptr.address(), log_hex(method_id), in_size, out_size.address());
    if (result)
        *result.get(emuenv.mem) = 0;
    if (out_size)
        *out_size.get(emuenv.mem) = 0;
    return 0;
}

EXPORT(int, _ZN4IPMI6Client17invokeAsyncMethodEjPKNS_8DataInfoEjPjPKNS0_12EventNotifeeE, Ptr<void> thisptr, uint32_t method_id, Ptr<void> in_data, uint32_t in_num, Ptr<uint32_t> req_id, Ptr<void> notifee) {
    LOG_INFO("[IPMI-DIAG] invokeAsyncMethod(DataInfo) this={} method={} inNum={}", thisptr.address(), log_hex(method_id), in_num);
    if (req_id)
        *req_id.get(emuenv.mem) = 1;
    return 0;
}

EXPORT(int, _ZN4IPMI6Client17invokeAsyncMethodEjPKvjPiPKNS0_12EventNotifeeE, Ptr<void> thisptr, uint32_t method_id, Ptr<void> in_data, uint32_t in_size, Ptr<int> req_id, Ptr<void> notifee) {
    LOG_INFO("[IPMI-DIAG] invokeAsyncMethod this={} method={} inSize={}", thisptr.address(), log_hex(method_id), in_size);
    if (req_id)
        *req_id.get(emuenv.mem) = 1;
    return 0;
}

EXPORT(int, _ZN4IPMI6Client19terminateConnectionEv) {
    return UNIMPLEMENTED();
}

// IPMI::Client::Config::estimateClientMemorySize()
EXPORT(int, _ZN4IPMI6Client6Config24estimateClientMemorySizeEv) {
    TRACY_FUNC(_ZN4IPMI6Client6Config24estimateClientMemorySizeEv);
    STUBBED("stubbed");
    return 0x100;
}

// IPMI::Client::create(IPMI::Client**, IPMI::Client::Config const*, void*, void*)
EXPORT(int, _ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_, Ptr<void> *client, void const *config, Ptr<void> user_data, Ptr<void> client_memory) {
    TRACY_FUNC(_ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_, client, config, user_data, client_memory);
    *client_memory.cast<Ptr<void>>().get(emuenv.mem) = get_client_vtable(emuenv.kernel, emuenv.mem);
    *client = client_memory;
    STUBBED("Stubed");
    return 0;
}

EXPORT(int, _ZN4IPMI6Client6getMsgEjPvPjjS2_, Ptr<void> thisptr, uint32_t a2, Ptr<void> buf, Ptr<uint32_t> size, uint32_t a5, Ptr<uint32_t> a6) {
    LOG_INFO("[IPMI-DIAG] getMsg this={} a2={}", thisptr.address(), a2);
    return UNIMPLEMENTED();
}

EXPORT(int, _ZN4IPMI6Client7connectEPKvjPi, void *client, void const *params, SceSize params_size, SceInt32 *error) {
    TRACY_FUNC(_ZN4IPMI6Client7connectEPKvjPi, client, error);
    LOG_INFO("[IPMI-DIAG] Client::connect paramsSize={}", params_size);
    *error = 0;
    return 0;
}

EXPORT(int, _ZN4IPMI6Client7destroyEv) {
    return UNIMPLEMENTED();
}

// No NP service daemon => never any pending message. The NP-infra worker loop
// (try_get_message) keeps polling while this returns 0 and breaks out on non-zero.
// Returning a "no message available" status stops the busy loop and lets the
// worker idle correctly.
EXPORT(int, _ZN4IPMI6Client9tryGetMsgEjPvPmm, Ptr<void> thisptr, uint32_t a2, Ptr<void> buf, Ptr<uint32_t> size, uint32_t a5) {
    return 0x80550C04; // SCE_NP_..._NO_MESSAGE (non-zero => no pending message)
}

EXPORT(int, _ZN4IPMI6ClientD1Ev) {
    return UNIMPLEMENTED();
}

// --- NP-infra IPMI client used by the real (LLE) np_manager/np_basic modules ---
// These are the connection-management entry points the firmware NP modules import
// from SceIpmi (distinct from the C++ IPMI::Client ABI above). The real ones talk
// to the on-device NP service daemon over IPMI; here we emulate a client whose
// connection always succeeds so the LLE NP stack can initialise in offline mode.

// IPMI client create: (Client **out, const Config *config, void *client_memory)
EXPORT(int, SceIpmi_22625598, Ptr<Ptr<void>> client_out, Ptr<void> config, Ptr<void> client_memory) {
    TRACY_FUNC(SceIpmi_22625598, client_out, config, client_memory);
    LOG_INFO("[IPMI-DIAG] create(out={}, config={}, mem={}) caller_lr={}", client_out.address(), config.address(), client_memory.address(), log_hex(ipmi_caller_lr(emuenv, thread_id)));
    if (client_out) {
        if (client_memory)
            *client_memory.cast<Ptr<void>>().get(emuenv.mem) = get_client_vtable(emuenv.kernel, emuenv.mem);
        *client_out.get(emuenv.mem) = client_memory;
    }
    return 0;
}

// IPMI client connect: (Client *client) -> must return >= 0 for NP init to proceed
EXPORT(int, SceIpmi_B56BAC7F, Ptr<void> client, Ptr<void> a2, uint32_t a3, Ptr<int> a4) {
    TRACY_FUNC(SceIpmi_B56BAC7F, client);
    LOG_INFO("[IPMI-DIAG] connect(client={}) caller_lr={}", client.address(), log_hex(ipmi_caller_lr(emuenv, thread_id)));
    if (a4)
        *a4.get(emuenv.mem) = 0;
    return 0;
}

// IPMI client disconnect
EXPORT(int, SceIpmi_B943F5D9, Ptr<void> client) {
    TRACY_FUNC(SceIpmi_B943F5D9, client);
    LOG_INFO("[IPMI-DIAG] disconnect(client={})", client.address());
    return 0;
}

// IPMI client destroy
EXPORT(int, SceIpmi_D73C03D2, Ptr<void> client) {
    TRACY_FUNC(SceIpmi_D73C03D2, client);
    LOG_INFO("[IPMI-DIAG] destroy(client={}) caller_lr={}", client.address(), log_hex(ipmi_caller_lr(emuenv, thread_id)));
    return 0;
}

// IPMI client process/pump (called at the start of NP RPC wrappers)
EXPORT(int, SceIpmi_9EE9496B, Ptr<void> client, uint32_t a2, uint32_t a3, uint32_t a4) {
    TRACY_FUNC(SceIpmi_9EE9496B, client);
    // Returns the client/connection object. np_basic's setup treats a zero return
    // as "connection not established" and aborts (0x80551D01); the NP-infra worker
    // loop uses it as the client handle for tryGetMsg. Return the (non-zero) client.
    return client ? static_cast<int>(client.address()) : 1;
}

EXPORT(int, SceIpmi_296D44D4, Ptr<void> a1, Ptr<void> a2, uint32_t a3, uint32_t a4) {
    LOG_INFO("[IPMI-DIAG] fn_296D44D4(a1={}, a2={}, a3={}, a4={})", a1.address(), a2.address(), a3, a4);
    return 0;
}

// Secondary IPMI client create (used by SceNpManagerIpc setup): (Client **out, config, mem)
EXPORT(int, SceIpmi_BC3A3031, Ptr<Ptr<void>> client_out, Ptr<void> config, Ptr<void> client_memory) {
    LOG_INFO("[IPMI-DIAG] create2(out={}, config={}, mem={})", client_out.address(), config.address(), client_memory.address());
    if (client_out) {
        if (client_memory)
            *client_memory.cast<Ptr<void>>().get(emuenv.mem) = get_client_vtable(emuenv.kernel, emuenv.mem);
        *client_out.get(emuenv.mem) = client_memory;
    }
    return 0;
}

// connect/register for the secondary client
EXPORT(int, SceIpmi_2C6DB642, Ptr<void> client) {
    LOG_INFO("[IPMI-DIAG] fn_2C6DB642(client={})", client.address());
    return 0;
}

// set-name / config helper (void-ish)
EXPORT(int, SceIpmi_006EFA9D, Ptr<void> a1, Ptr<void> a2, uint32_t a3) {
    return 0;
}

// destroy for the secondary client (failure/teardown path)
EXPORT(int, SceIpmi_A20BB95B, Ptr<void> client) {
    LOG_INFO("[IPMI-DIAG] fn_A20BB95B(client={})", client.address());
    return 0;
}
