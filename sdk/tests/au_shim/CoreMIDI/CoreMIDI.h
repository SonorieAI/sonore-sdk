// SPDX-License-Identifier: Apache-2.0
// See ../README.md. The AU wrapper names CoreMIDI types for its MIDI output
// callback; it does not open a CoreMIDI port itself.
#pragma once
#include <CoreFoundation/CoreFoundation.h>

typedef UInt32 MIDIObjectRef;
typedef MIDIObjectRef MIDIEndpointRef;
typedef UInt32 ItemCount;
typedef UInt64 MIDITimeStamp;
typedef unsigned long ByteCount;

struct MIDIPacket {
  MIDITimeStamp timeStamp;
  UInt16 length;
  UInt8 data[256];
};

struct MIDIPacketList {
  UInt32 numPackets;
  MIDIPacket packet[1];
};

MIDIPacket* MIDIPacketListInit(MIDIPacketList*);
MIDIPacket* MIDIPacketListAdd(MIDIPacketList*, ByteCount, MIDIPacket*, MIDITimeStamp, ByteCount,
                              const UInt8*);

typedef MIDIObjectRef MIDIClientRef;
typedef MIDIObjectRef MIDIPortRef;
typedef MIDIObjectRef MIDIDeviceRef;

typedef void (*MIDINotifyProc)(const struct MIDINotification* message, void* refCon);
typedef void (*MIDIReadProc)(const MIDIPacketList* pktlist, void* readProcRefCon,
                             void* srcConnRefCon);

struct MIDINotification {
  UInt32 messageID;
  UInt32 messageSize;
};

extern const CFStringRef kMIDIPropertyDisplayName;
extern const CFStringRef kMIDIPropertyName;

ItemCount MIDIGetNumberOfSources(void);
MIDIEndpointRef MIDIGetSource(ItemCount);
OSStatus MIDIClientCreate(CFStringRef, MIDINotifyProc, void*, MIDIClientRef*);
OSStatus MIDIClientDispose(MIDIClientRef);
OSStatus MIDIInputPortCreate(MIDIClientRef, CFStringRef, MIDIReadProc, void*, MIDIPortRef*);
OSStatus MIDIPortDispose(MIDIPortRef);
OSStatus MIDIPortConnectSource(MIDIPortRef, MIDIEndpointRef, void*);
OSStatus MIDIPortDisconnectSource(MIDIPortRef, MIDIEndpointRef);
OSStatus MIDIObjectGetStringProperty(MIDIObjectRef, CFStringRef, CFStringRef*);
const MIDIPacket* MIDIPacketNext(const MIDIPacket*);
