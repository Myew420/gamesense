#pragma once

#include <cstdint>
#include <cstring>

#include <CS2/Constants/DllNames.h>
#include <Platform/DynamicLibrary.h>

// resolves the live CNetworkGameClient via the engine2 global cs2-dumper records as
// dwNetworkGameClient (raw RVA, so the object is validated before every use). freed on
// disconnect, so the pointer is never cached; only the module base + vmt range are
// (getVmtSection() mmaps, never per tick).
// layout: tick field = +0x3A8 (was +0x388 pre-5GB-update); channel array = +0xF0,
// 24-byte stride, GetChannel(slot) = vtable slot 41.
struct NetworkGameClientPointer {
    NetworkGameClientPointer() noexcept
        : pointer{resolve()}
    {
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return pointer != nullptr;
    }

    [[nodiscard]] void* get() const noexcept
    {
        return pointer;
    }

    // fail-closed sanity: vtable must sit in libengine2's vmt range and the tick field
    // must read as a plausible tick (guards against a stale RVA after a game update).
    [[nodiscard]] bool valid() const noexcept
    {
        return validate(pointer);
    }

private:
    [[nodiscard]] static bool validate(void* client) noexcept
    {
        if (!client)
            return false;
        // reject non-canonical pointers first: a stale RVA can read a non-canonical
        // .data qword, and dereferencing it for the vtable check is a #GP before any
        // other sanity runs.
        const auto address = reinterpret_cast<std::uintptr_t>(client);
        if (address >= 0x0000800000000000ULL && address < 0xffff800000000000ULL)
            return false;
        void* vtable = nullptr;
        std::memcpy(&vtable, client, sizeof(vtable));
        if (!engineVmtContains(std::uintptr_t(vtable)))
            return false;
        const auto tick = *reinterpret_cast<const volatile std::int32_t*>(
            reinterpret_cast<std::uintptr_t>(client) + 0x3A8);
        return tick >= 0 && tick < (1 << 24);
    }

    // Module base + vmt range, cached once (both are immutable after load).
    [[nodiscard]] static bool engineVmtContains(std::uintptr_t address) noexcept
    {
        static std::uintptr_t cachedStart = 0;
        static std::uintptr_t cachedEnd = 0;
        if (cachedEnd == 0) {
            const DynamicLibrary engineDLL{cs2::ENGINE_DLL};
            const auto vmt = engineDLL.getVmtSection();
            const auto raw = vmt.raw();
            if (!raw.empty() && raw.data()) {
                cachedStart = reinterpret_cast<std::uintptr_t>(raw.data());
                cachedEnd = cachedStart + raw.size();
            }
        }
        return cachedEnd != 0 && address >= cachedStart && address < cachedEnd;
    }

    [[nodiscard]] static void* resolve() noexcept
    {
        const DynamicLibrary engineDLL{cs2::ENGINE_DLL};
        const auto linkMap = engineDLL.getLinkMap();
        if (!linkMap || !linkMap->l_addr)
            return nullptr;
        const auto base = reinterpret_cast<std::uintptr_t>(linkMap->l_addr);

        void* client = nullptr;
        std::memcpy(&client, reinterpret_cast<const void*>(base + kNetworkGameClientRva), sizeof(client));
        if (!validate(client))
            return nullptr;
        return client;
    }

    // libengine2.so dwNetworkGameClient RVA (previous value 0xA477C0; an update moved it
    // +0x200 and the stale RVA read a non-canonical qword). re-derive on any CS2 update;
    // valid() fails closed on drift.
    static constexpr std::uintptr_t kNetworkGameClientRva = 0xA479C0;

    void* pointer{nullptr};
};