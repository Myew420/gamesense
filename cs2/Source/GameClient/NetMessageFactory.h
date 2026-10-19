#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <CS2/Constants/DllNames.h>
#include <GameClient/NetworkMessagesPointer.h>

// send a real net message through the game's own net channel.
// chain: NetworkMessagesVersion001 (libnetworksystem) manager -> findRecord -> info ->
// binding -> create -> ReadFromBuffer -> CNetChan SendNetMessage -> commit -> SendData.
// slot indexes in the kSlot* constants below (byte offsets = slot * 8); windows vcall
// numbers do not map 1:1 (MSVC vs Itanium). every vtable is validated against the owning
// module before any call; failures return null/false and features latch fail closed.
namespace net_messages
{

// engine bit_read_t layout: data_bits @ +0xC, current_bit @ +0x10, overflow @ +0x20.
struct BitRead {
    const void* data;
    std::int32_t dataBytes;
    std::int32_t dataBits;
    std::int32_t currentBit;
    std::uint32_t reserved;
    const char* debugName;
    bool overflow;
    bool initialized;
    bool dwordSafe;
    std::uint8_t tail[5];
};
static_assert(sizeof(BitRead) == 0x28);

// serialize-size sanity ceiling; bigger = the parse corrupted a length field.
inline constexpr std::uint32_t kMaxSaneSerializeBytes = 32 * 1024;

// Verified Linux slot indexes (byte offsets = slot * 8).
inline constexpr std::size_t kSlotFindRecord = 30 * 8;      // CNetworkMessages: registry walk by message type id
inline constexpr std::size_t kSlotGetInfo = 12 * 8;         // CNetworkMessages: info = record+0x10
inline constexpr std::size_t kSlotReadFromBuffer = 4 * 8;   // CNetworkMessages: parse bit_read_t into message
inline constexpr std::size_t kSlotBindingCreate = 6 * 8;    // CProtobufBinding: new 0x60-byte message
inline constexpr std::size_t kSlotMessageClone = 5 * 8;     // CNetMessagePB copy ctor
inline constexpr std::size_t kSlotMessageDestroy = 1 * 8;   // Itanium D2 deleting dtor
inline constexpr std::size_t kSlotChannelSend = 40 * 8;     // CNetChan::SendNetMessage(msg, flag)
inline constexpr std::size_t kSlotChannelCommit = 43 * 8;   // CNetChan: commit all 3 stream buffers
inline constexpr std::size_t kSlotChannelSendData = 42 * 8; // CNetChan: build+send datagram from committed
inline constexpr std::size_t kSlotChannelReady = 47 * 8;    // CNetChan: send-window check
inline constexpr std::size_t kSlotClientGetChannel = 41 * 8;// CNetworkGameClient::GetChannel(slot)

// GetTick (vtable slot 5) reads [this+0x3A8]; was +0x388 before the 5GB update.
inline constexpr std::uintptr_t kClientTickFieldOffset = 0x3A8;

using FindRecordFn = void* (*)(void* manager, int messageId);
using GetInfoFn = void* (*)(void* manager, void* record);
using ReadFromBufferFn = bool (*)(void* manager, BitRead* reader, void* message);
using CreateMessageFn = void* (*)(void* binding);
using CloneMessageFn = void* (*)(void* message);
using DestroyMessageFn = void (*)(void* message);
using SendNetMessageFn = bool (*)(void* channel, void* message, std::int8_t flag);
using CommitChannelFn = void (*)(void* channel);
using SendDataFn = std::int32_t (*)(void* channel, const void* descriptor, void* argument);
using ChannelReadyFn = bool (*)(void* channel);
using GetChannelFn = void* (*)(void* networkClient, int slot);

[[nodiscard]] inline void* vtableSlot(void* object, std::size_t byteOffset) noexcept
{
    void* vtable = *reinterpret_cast<void**>(object);
    return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(vtable) + byteOffset);
}

inline void appendVarint(std::uint8_t* out, std::size_t& written, std::uint32_t value) noexcept
{
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7Fu);
        value >>= 7u;
        if (value)
            byte |= 0x80u;
        out[written++] = byte;
    } while (value);
}

[[nodiscard]] inline std::size_t varintLength(std::uint32_t value) noexcept
{
    std::size_t length = 1;
    while (value >= 0x80u) {
        value >>= 7u;
        ++length;
    }
    return length;
}

// The protobuf sub-object lives at message+0x30 (embedded). Its vtable slot 8 (byte 0x40) is
// ByteSize - the same call the engine's own serialize path makes before malloc'ing the output.
[[nodiscard]] inline std::uint32_t protoByteSize(void* message) noexcept
{
    if (!message)
        return 0;
    void* proto = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(message) + 0x30);
    void* protoVtable = *reinterpret_cast<void**>(proto);
    if (!protoVtable)
        return 0;
    const auto byteSizeFn = *reinterpret_cast<std::uint32_t (**)(void*)>(reinterpret_cast<std::uintptr_t>(protoVtable) + 0x40);
    return byteSizeFn ? byteSizeFn(proto) : 0;
}

inline void destroyMessage(void* message) noexcept
{
    if (!message)
        return;
    const auto destroyFn = reinterpret_cast<DestroyMessageFn>(vtableSlot(message, kSlotMessageDestroy));
    if (destroyFn)
        destroyFn(message);
}

// manager -> record(typeId) -> info -> binding; null on any failure. no caching needed
// (hash lookup, not a scan).
[[nodiscard]] inline void* resolveMessageBinding(int recordId) noexcept
{
    const NetworkMessagesPointer messagesPointer{};
    if (!messagesPointer)
        return nullptr;
    void* const manager = messagesPointer.get();

    const auto findRecord = reinterpret_cast<FindRecordFn>(vtableSlot(manager, kSlotFindRecord));
    void* const record = findRecord ? findRecord(manager, recordId) : nullptr;
    if (!record)
        return nullptr;

    const auto getInfo = reinterpret_cast<GetInfoFn>(vtableSlot(manager, kSlotGetInfo));
    void* const info = getInfo ? getInfo(manager, record) : nullptr;
    if (!info)
        return nullptr;

    return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(info) + 8);
}

// stage string of the last makeMessage failure (null = success). game-thread only.
inline const char* lastMakeMessageFailure = nullptr;

// create a message for `recordId`, parse `payload` into it. framed form fed to ReadFromBuffer:
// varint(payloadSize) + payload + 4 zero slack bytes. non-null return must be destroyMessage()'d.
[[nodiscard]] inline void* makeMessage(int recordId, const void* payload, std::size_t payloadSize,
                                       const char* debugName, std::uint8_t* framedScratch, std::size_t scratchSize) noexcept
{
    void* const binding = resolveMessageBinding(recordId);
    if (!binding) {
        lastMakeMessageFailure = "record/binding unresolved";
        return nullptr;
    }

    const auto createFn = reinterpret_cast<CreateMessageFn>(vtableSlot(binding, kSlotBindingCreate));
    void* const message = createFn ? createFn(binding) : nullptr;
    if (!message) {
        lastMakeMessageFailure = "binding create failed";
        return nullptr;
    }

    const NetworkMessagesPointer messagesPointer{};
    if (!messagesPointer) {
        destroyMessage(message);
        lastMakeMessageFailure = "message factory unavailable";
        return nullptr;
    }

    std::size_t written = 0;
    appendVarint(framedScratch, written, static_cast<std::uint32_t>(payloadSize));
    if (written + payloadSize + 4 > scratchSize) {
        destroyMessage(message);
        lastMakeMessageFailure = "payload does not fit scratch";
        return nullptr;
    }
    std::memcpy(framedScratch + written, payload, payloadSize);
    written += payloadSize;
    const std::size_t logicalSize = written; // varint + payload exactly
    std::memset(framedScratch + written, 0, 4);
    written += 4;

    BitRead reader{
        framedScratch,
        static_cast<std::int32_t>(logicalSize),
        static_cast<std::int32_t>(logicalSize * 8),
        0,
        0,
        debugName,
        false,
        true,
        true,
        {},
    };

    const auto readFromBuffer = reinterpret_cast<ReadFromBufferFn>(vtableSlot(messagesPointer.get(), kSlotReadFromBuffer));
    const bool parsed = readFromBuffer && readFromBuffer(messagesPointer.get(), &reader, message) && !reader.overflow;
    if (!parsed) {
        destroyMessage(message);
        lastMakeMessageFailure = reader.overflow ? "parse overflow" : "parse failed";
        return nullptr;
    }

    const auto byteSize = protoByteSize(message);
    if (byteSize == 0 || byteSize > kMaxSaneSerializeBytes) {
        destroyMessage(message);
        lastMakeMessageFailure = "serialize size gate";
        return nullptr;
    }
    lastMakeMessageFailure = nullptr;
    return message;
}

// Validates + returns the client's CNetChan for slot 0, or null (not connected / stale object).
[[nodiscard]] inline void* getClientChannel(void* networkClient) noexcept
{
    if (!networkClient)
        return nullptr;
    const auto getChannelFn = reinterpret_cast<GetChannelFn>(vtableSlot(networkClient, kSlotClientGetChannel));
    void* const channel = getChannelFn ? getChannelFn(networkClient, 0) : nullptr;
    if (!channel || !NetworkMessagesPointer::vtableInNetworkSystemModule(reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void**>(channel))))
        return nullptr;
    return channel;
}

[[nodiscard]] inline bool channelReady(void* channel) noexcept
{
    const auto readyFn = reinterpret_cast<ChannelReadyFn>(vtableSlot(channel, kSlotChannelReady));
    return readyFn && readyFn(channel);
}

// clone via Itanium copy ctor (slot 5; slot 4 is a getter on linux). the clone is the
// caller's to destroy with destroyMessage().
[[nodiscard]] inline void* cloneMessage(void* message) noexcept
{
    if (!message)
        return nullptr;
    const auto cloneFn = reinterpret_cast<CloneMessageFn>(vtableSlot(message, kSlotMessageClone));
    return cloneFn ? cloneFn(message) : nullptr;
}

// Queues a message object into the channel's send buffer. Returns false when refused (buffer
// full backpressure) - the message is still the caller's to destroy.
inline bool sendNetMessage(void* channel, void* message) noexcept
{
    const auto sendFn = reinterpret_cast<SendNetMessageFn>(vtableSlot(channel, kSlotChannelSend));
    return sendFn && sendFn(channel, message, -1);
}

// Seals the queued message bits into the outgoing stream (call after a batch of sends).
inline void commitChannel(void* channel) noexcept
{
    const auto commitFn = reinterpret_cast<CommitChannelFn>(vtableSlot(channel, kSlotChannelCommit));
    if (commitFn)
        commitFn(channel);
}

// builds + transmits a datagram from committed data; returns byte count (0 = refused,
// refused data stays queued so callers must back off).
inline std::int32_t transmitChannelEx(void* channel, const char* debugName) noexcept
{
    const auto sendDataFn = reinterpret_cast<SendDataFn>(vtableSlot(channel, kSlotChannelSendData));
    return sendDataFn ? sendDataFn(channel, debugName, nullptr) : 0;
}

inline bool transmitChannel(void* channel, const char* debugName) noexcept
{
    return transmitChannelEx(channel, debugName) != 0;
}

[[nodiscard]] inline std::int32_t networkClientTick(void* networkClient) noexcept
{
    return *reinterpret_cast<const volatile std::int32_t*>(
        reinterpret_cast<std::uintptr_t>(networkClient) + kClientTickFieldOffset);
}

// --- CNETMsg_SetConVar payload builder -----------------------------------------------------
// CNETMsg_SetConVar { optional CMsg_CVars convars = 1; }
// CMsg_CVars { repeated CVar cvars = 1; }  CVar { optional string name = 1; optional string value = 2; }
inline constexpr int kSetConVarMessageId = 6; // NET group id (N13CNetMessagePBILi6E17CNETMsg_SetConVar in the RTTI catalog)

struct SetConVarEntry {
    const char* name;
    const char* value;
};

// Writes the wire form of a SetConVar message carrying all `entries`. Returns the byte count,
// or 0 when the entries do not fit `outCap` (or count/outCap degenerate).
[[nodiscard]] inline std::size_t makeSetConVarPayload(const SetConVarEntry* entries, int count,
                                                      std::uint8_t* out, std::size_t outCap) noexcept
{
    if (!entries || count <= 0 || !out || outCap == 0)
        return 0;

    std::size_t cvarsLength = 0;
    for (int i = 0; i < count; ++i) {
        const std::size_t nameLength = entries[i].name ? std::strlen(entries[i].name) : 0;
        const std::size_t valueLength = entries[i].value ? std::strlen(entries[i].value) : 0;
        if (nameLength == 0)
            return 0;
        // entry = 0x0A varint(len(name)) name 0x12 varint(len(value)) value
        const std::size_t entryLength = 1 + varintLength(static_cast<std::uint32_t>(nameLength)) + nameLength
            + 1 + varintLength(static_cast<std::uint32_t>(valueLength)) + valueLength;
        // repeated field header = 0x0A varint(entryLength)
        cvarsLength += 1 + varintLength(static_cast<std::uint32_t>(entryLength)) + entryLength;
    }

    const std::size_t totalLength = 1 + varintLength(static_cast<std::uint32_t>(cvarsLength)) + cvarsLength;
    if (totalLength > outCap)
        return 0;

    std::size_t written = 0;
    out[written++] = 0x0A; // convars = 1, length-delimited
    appendVarint(out, written, static_cast<std::uint32_t>(cvarsLength));
    for (int i = 0; i < count; ++i) {
        const auto* name = reinterpret_cast<const std::uint8_t*>(entries[i].name);
        const auto* value = reinterpret_cast<const std::uint8_t*>(entries[i].value);
        const std::size_t nameLength = entries[i].name ? std::strlen(entries[i].name) : 0;
        const std::size_t valueLength = entries[i].value ? std::strlen(entries[i].value) : 0;
        const std::size_t entryLength = 1 + varintLength(static_cast<std::uint32_t>(nameLength)) + nameLength
            + 1 + varintLength(static_cast<std::uint32_t>(valueLength)) + valueLength;

        out[written++] = 0x0A; // cvars = 1, length-delimited
        appendVarint(out, written, static_cast<std::uint32_t>(entryLength));
        out[written++] = 0x0A; // name = 1
        appendVarint(out, written, static_cast<std::uint32_t>(nameLength));
        std::memcpy(out + written, name, nameLength);
        written += nameLength;
        out[written++] = 0x12; // value = 2
        appendVarint(out, written, static_cast<std::uint32_t>(valueLength));
        std::memcpy(out + written, value, valueLength);
        written += valueLength;
    }
    return written;
}

// sends one SetConVar entry: queue -> commit -> transmit, so the update leaves as its own
// datagram. bypasses the engine's userinfo-change machinery (which coalesces churn).
// value must fit 512 bytes; on failure *failReason names the stage.
[[nodiscard]] inline bool sendSingleSetConVar(void* networkClient, const char* cvarName, const char* value, const char* debugName, const char** failReason = nullptr) noexcept
{
    void* const channel = getClientChannel(networkClient);
    if (!channel) {
        if (failReason)
            *failReason = "no net channel";
        return false;
    }
    if (!channelReady(channel)) {
        if (failReason)
            *failReason = "send window closed";
        return false;
    }

    const SetConVarEntry entry{cvarName, value};
    std::uint8_t payload[512];
    const std::size_t payloadSize = makeSetConVarPayload(&entry, 1, payload, sizeof(payload));
    if (payloadSize == 0) {
        if (failReason)
            *failReason = "value too long";
        return false;
    }

    std::uint8_t framed[528];
    void* const message = makeMessage(kSetConVarMessageId, payload, payloadSize, debugName, framed, sizeof(framed));
    if (!message) {
        if (failReason)
            *failReason = lastMakeMessageFailure ? lastMakeMessageFailure : "factory failed";
        return false;
    }
    const bool accepted = sendNetMessage(channel, message);
    destroyMessage(message);
    if (!accepted) {
        if (failReason)
            *failReason = "channel refused message";
        return false;
    }

    commitChannel(channel);
    if (!transmitChannel(channel, debugName)) {
        if (failReason)
            *failReason = "datagram transmit refused";
        return false;
    }
    return true;
}

} // namespace net_messages
