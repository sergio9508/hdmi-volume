#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dsp_loopback.h"

#define kPlugIn_BundleID "com.hdmivolume.driver"
#define kDevice_UID      "com.hdmivolume.driver.device"
#define kDevice_ModelUID "com.hdmivolume.driver.model"

enum {
    kObjectID_PlugIn               = kAudioObjectPlugInObject,
    kObjectID_Device               = 2,
    kObjectID_Stream_Input         = 3,
    kObjectID_Stream_Output        = 4,
    kObjectID_Volume_Output_Master = 5,
    kObjectID_Mute_Output_Master   = 6
};

static const Float64 kSupportedSampleRates[] = {44100.0, 48000.0, 96000.0};
static const UInt32 kNumSupportedSampleRates = 3;
static const UInt32 kZeroTimestampPeriod = 4096;

typedef struct {
    AudioServerPlugInDriverInterface *interface;
    _Atomic UInt32 ref_count;
    AudioServerPlugInHostRef host;
    pthread_mutex_t state_mutex;

    Float64 sample_rate;
    UInt32 io_client_count;
    UInt64 anchor_host_time;
    Float64 host_ticks_per_frame;

    _Atomic float volume_scalar;
    _Atomic bool muted;
    hdv_ring_buffer_t ring;
} HDMIVolumePlugInState;

static HDMIVolumePlugInState gState;
static bool gStateInitialized = false;

static void update_host_ticks_per_frame(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    Float64 ticks_per_sec = (Float64)tb.denom / (Float64)tb.numer * 1.0e9;
    gState.host_ticks_per_frame = ticks_per_sec / gState.sample_rate;
}

static Float32 scalar_to_db(Float32 scalar) {
    if (scalar <= 0.0f) return -60.0f;
    if (scalar >= 1.0f) return 0.0f;
    return (scalar - 1.0f) * 60.0f;
}

static Float32 db_to_scalar(Float32 db) {
    if (db <= -60.0f) return 0.0f;
    if (db >= 0.0f) return 1.0f;
    return (db / 60.0f) + 1.0f;
}

static void notify_host_property_changed(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
    if (gState.host && gState.host->PropertiesChanged) {
        AudioObjectPropertyAddress addr = {sel, scope, kAudioObjectPropertyElementMain};
        gState.host->PropertiesChanged(gState.host, obj, 1, &addr);
    }
}

static HRESULT HDV_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    if (!outInterface) return kAudioHardwareIllegalOperationError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    CFUUIDRef iunknown = IUnknownUUID;
    CFUUIDRef plugin_if = kAudioServerPlugInDriverInterfaceUUID;
    Boolean match = CFEqual(requested, iunknown) || CFEqual(requested, plugin_if);
    CFRelease(requested);
    if (match) {
        atomic_fetch_add(&gState.ref_count, 1);
        *outInterface = inDriver;
        return kAudioHardwareNoError;
    }
    *outInterface = NULL;
    return E_NOINTERFACE;
}

static ULONG HDV_AddRef(void *inDriver) {
    (void)inDriver;
    return (ULONG)atomic_fetch_add(&gState.ref_count, 1) + 1;
}

static ULONG HDV_Release(void *inDriver) {
    (void)inDriver;
    UInt32 prev = atomic_fetch_sub(&gState.ref_count, 1);
    return (ULONG)(prev > 0 ? prev - 1 : 0);
}

static OSStatus HDV_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    (void)inDriver;
    pthread_mutex_lock(&gState.state_mutex);
    gState.host = inHost;
    update_host_ticks_per_frame();
    pthread_mutex_unlock(&gState.state_mutex);
    return kAudioHardwareNoError;
}

static OSStatus HDV_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID) {
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus HDV_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) {
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus HDV_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return kAudioHardwareNoError;
}

static OSStatus HDV_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return kAudioHardwareNoError;
}

static OSStatus HDV_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeInfo;
    pthread_mutex_lock(&gState.state_mutex);
    Float64 new_rate = (Float64)inChangeAction;
    for (UInt32 i = 0; i < kNumSupportedSampleRates; ++i) {
        if (kSupportedSampleRates[i] == new_rate) {
            gState.sample_rate = new_rate;
            update_host_ticks_per_frame();
            hdv_ring_reset(&gState.ring);
            break;
        }
    }
    pthread_mutex_unlock(&gState.state_mutex);
    return kAudioHardwareNoError;
}

static OSStatus HDV_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return kAudioHardwareNoError;
}

static Boolean HDV_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress) {
    (void)inDriver; (void)inClientProcessID;
    if (!inAddress) return false;
    AudioObjectPropertySelector sel = inAddress->mSelector;
    AudioObjectPropertyScope scope = inAddress->mScope;

    switch (inObjectID) {
        case kObjectID_PlugIn:
            return (sel == kAudioObjectPropertyBaseClass ||
                    sel == kAudioObjectPropertyClass ||
                    sel == kAudioObjectPropertyOwner ||
                    sel == kAudioObjectPropertyManufacturer ||
                    sel == kAudioObjectPropertyOwnedObjects ||
                    sel == kAudioPlugInPropertyDeviceList ||
                    sel == kAudioPlugInPropertyTranslateUIDToDevice ||
                    sel == kAudioPlugInPropertyResourceBundle);
        case kObjectID_Device:
            return (sel == kAudioObjectPropertyBaseClass ||
                    sel == kAudioObjectPropertyClass ||
                    sel == kAudioObjectPropertyOwner ||
                    sel == kAudioObjectPropertyName ||
                    sel == kAudioObjectPropertyManufacturer ||
                    sel == kAudioObjectPropertyOwnedObjects ||
                    sel == kAudioDevicePropertyDeviceUID ||
                    sel == kAudioDevicePropertyModelUID ||
                    sel == kAudioDevicePropertyTransportType ||
                    sel == kAudioDevicePropertyRelatedDevices ||
                    sel == kAudioDevicePropertyClockDomain ||
                    sel == kAudioDevicePropertyDeviceIsAlive ||
                    sel == kAudioDevicePropertyDeviceIsRunning ||
                    sel == kAudioDevicePropertyDeviceCanBeDefaultDevice ||
                    sel == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice ||
                    sel == kAudioDevicePropertyLatency ||
                    sel == kAudioDevicePropertyStreams ||
                    sel == kAudioObjectPropertyControlList ||
                    sel == kAudioDevicePropertySafetyOffset ||
                    sel == kAudioDevicePropertyNominalSampleRate ||
                    sel == kAudioDevicePropertyAvailableNominalSampleRates ||
                    sel == kAudioDevicePropertyIsHidden ||
                    sel == kAudioDevicePropertyZeroTimeStampPeriod ||
                    (sel == kAudioDevicePropertyPreferredChannelsForStereo &&
                     (scope == kAudioObjectPropertyScopeInput || scope == kAudioObjectPropertyScopeOutput)) ||
                    (sel == kAudioDevicePropertyPreferredChannelLayout &&
                     (scope == kAudioObjectPropertyScopeInput || scope == kAudioObjectPropertyScopeOutput)));
        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            return (sel == kAudioObjectPropertyBaseClass ||
                    sel == kAudioObjectPropertyClass ||
                    sel == kAudioObjectPropertyOwner ||
                    sel == kAudioStreamPropertyIsActive ||
                    sel == kAudioStreamPropertyDirection ||
                    sel == kAudioStreamPropertyTerminalType ||
                    sel == kAudioStreamPropertyStartingChannel ||
                    sel == kAudioStreamPropertyLatency ||
                    sel == kAudioStreamPropertyVirtualFormat ||
                    sel == kAudioStreamPropertyPhysicalFormat ||
                    sel == kAudioStreamPropertyAvailableVirtualFormats ||
                    sel == kAudioStreamPropertyAvailablePhysicalFormats);
        case kObjectID_Volume_Output_Master:
            return (sel == kAudioObjectPropertyBaseClass ||
                    sel == kAudioObjectPropertyClass ||
                    sel == kAudioObjectPropertyOwner ||
                    sel == kAudioControlPropertyScope ||
                    sel == kAudioControlPropertyElement ||
                    sel == kAudioLevelControlPropertyScalarValue ||
                    sel == kAudioLevelControlPropertyDecibelValue ||
                    sel == kAudioLevelControlPropertyDecibelRange ||
                    sel == kAudioLevelControlPropertyConvertScalarToDecibels ||
                    sel == kAudioLevelControlPropertyConvertDecibelsToScalar);
        case kObjectID_Mute_Output_Master:
            return (sel == kAudioObjectPropertyBaseClass ||
                    sel == kAudioObjectPropertyClass ||
                    sel == kAudioObjectPropertyOwner ||
                    sel == kAudioControlPropertyScope ||
                    sel == kAudioControlPropertyElement ||
                    sel == kAudioBooleanControlPropertyValue);
        default:
            return false;
    }
}

static OSStatus HDV_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable) {
    (void)inDriver; (void)inClientProcessID;
    if (!inAddress || !outIsSettable) return kAudioHardwareIllegalOperationError;
    *outIsSettable = false;

    if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
        *outIsSettable = true;
    } else if (inObjectID == kObjectID_Volume_Output_Master &&
               (inAddress->mSelector == kAudioLevelControlPropertyScalarValue ||
                inAddress->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        *outIsSettable = true;
    } else if (inObjectID == kObjectID_Mute_Output_Master &&
               inAddress->mSelector == kAudioBooleanControlPropertyValue) {
        *outIsSettable = true;
    }
    return kAudioHardwareNoError;
}

static OSStatus HDV_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize) {
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (!inAddress || !outDataSize) return kAudioHardwareIllegalOperationError;
    AudioObjectPropertySelector sel = inAddress->mSelector;
    AudioObjectPropertyScope scope = inAddress->mScope;

    switch (inObjectID) {
        case kObjectID_PlugIn:
            if (sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass || sel == kAudioObjectPropertyOwner) {
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyManufacturer || sel == kAudioPlugInPropertyResourceBundle) {
                *outDataSize = sizeof(CFStringRef);
            } else if (sel == kAudioObjectPropertyOwnedObjects || sel == kAudioPlugInPropertyDeviceList || sel == kAudioPlugInPropertyTranslateUIDToDevice) {
                *outDataSize = sizeof(AudioObjectID);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;

        case kObjectID_Device:
            if (sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass || sel == kAudioObjectPropertyOwner ||
                sel == kAudioDevicePropertyTransportType || sel == kAudioDevicePropertyClockDomain ||
                sel == kAudioDevicePropertyDeviceIsAlive || sel == kAudioDevicePropertyDeviceIsRunning ||
                sel == kAudioDevicePropertyDeviceCanBeDefaultDevice || sel == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice ||
                sel == kAudioDevicePropertyLatency || sel == kAudioDevicePropertySafetyOffset ||
                sel == kAudioDevicePropertyIsHidden || sel == kAudioDevicePropertyZeroTimeStampPeriod) {
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioObjectPropertyName || sel == kAudioObjectPropertyManufacturer ||
                       sel == kAudioDevicePropertyDeviceUID || sel == kAudioDevicePropertyModelUID) {
                *outDataSize = sizeof(CFStringRef);
            } else if (sel == kAudioObjectPropertyOwnedObjects) {
                *outDataSize = 4 * sizeof(AudioObjectID);
            } else if (sel == kAudioDevicePropertyRelatedDevices) {
                *outDataSize = sizeof(AudioObjectID);
            } else if (sel == kAudioDevicePropertyStreams) {
                if (scope == kAudioObjectPropertyScopeInput || scope == kAudioObjectPropertyScopeOutput) {
                    *outDataSize = sizeof(AudioObjectID);
                } else {
                    *outDataSize = 2 * sizeof(AudioObjectID);
                }
            } else if (sel == kAudioObjectPropertyControlList) {
                *outDataSize = 2 * sizeof(AudioObjectID);
            } else if (sel == kAudioDevicePropertyNominalSampleRate) {
                *outDataSize = sizeof(Float64);
            } else if (sel == kAudioDevicePropertyAvailableNominalSampleRates) {
                *outDataSize = kNumSupportedSampleRates * sizeof(AudioValueRange);
            } else if (sel == kAudioDevicePropertyPreferredChannelsForStereo) {
                *outDataSize = 2 * sizeof(UInt32);
            } else if (sel == kAudioDevicePropertyPreferredChannelLayout) {
                *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + 2 * sizeof(AudioChannelDescription);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            if (sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass || sel == kAudioObjectPropertyOwner ||
                sel == kAudioStreamPropertyIsActive || sel == kAudioStreamPropertyDirection ||
                sel == kAudioStreamPropertyTerminalType || sel == kAudioStreamPropertyStartingChannel ||
                sel == kAudioStreamPropertyLatency) {
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioStreamPropertyVirtualFormat || sel == kAudioStreamPropertyPhysicalFormat) {
                *outDataSize = sizeof(AudioStreamBasicDescription);
            } else if (sel == kAudioStreamPropertyAvailableVirtualFormats || sel == kAudioStreamPropertyAvailablePhysicalFormats) {
                *outDataSize = kNumSupportedSampleRates * sizeof(AudioStreamRangedDescription);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;

        case kObjectID_Volume_Output_Master:
            if (sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass || sel == kAudioObjectPropertyOwner ||
                sel == kAudioControlPropertyScope || sel == kAudioControlPropertyElement) {
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioLevelControlPropertyScalarValue || sel == kAudioLevelControlPropertyDecibelValue ||
                       sel == kAudioLevelControlPropertyConvertScalarToDecibels || sel == kAudioLevelControlPropertyConvertDecibelsToScalar) {
                *outDataSize = sizeof(Float32);
            } else if (sel == kAudioLevelControlPropertyDecibelRange) {
                *outDataSize = sizeof(AudioValueRange);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;

        case kObjectID_Mute_Output_Master:
            if (sel == kAudioObjectPropertyBaseClass || sel == kAudioObjectPropertyClass || sel == kAudioObjectPropertyOwner ||
                sel == kAudioControlPropertyScope || sel == kAudioControlPropertyElement ||
                sel == kAudioBooleanControlPropertyValue) {
                *outDataSize = sizeof(UInt32);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;

        default:
            return kAudioHardwareBadObjectError;
    }
}

static void fill_asbd(AudioStreamBasicDescription *asbd, Float64 rate) {
    asbd->mSampleRate = rate;
    asbd->mFormatID = kAudioFormatLinearPCM;
    asbd->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    asbd->mBytesPerPacket = sizeof(Float32) * HDV_CHANNELS;
    asbd->mFramesPerPacket = 1;
    asbd->mBytesPerFrame = sizeof(Float32) * HDV_CHANNELS;
    asbd->mChannelsPerFrame = HDV_CHANNELS;
    asbd->mBitsPerChannel = 32;
    asbd->mReserved = 0;
}

static OSStatus HDV_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (!inAddress || !outDataSize || !outData) return kAudioHardwareIllegalOperationError;
    AudioObjectPropertySelector sel = inAddress->mSelector;
    AudioObjectPropertyScope scope = inAddress->mScope;

    switch (inObjectID) {
        case kObjectID_PlugIn:
            if (sel == kAudioObjectPropertyBaseClass) {
                *((AudioClassID *)outData) = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyClass) {
                *((AudioClassID *)outData) = kAudioPlugInClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyOwner) {
                *((AudioObjectID *)outData) = kAudioObjectUnknown;
                *outDataSize = sizeof(AudioObjectID);
            } else if (sel == kAudioObjectPropertyManufacturer) {
                *((CFStringRef *)outData) = CFSTR("HDMI Volume");
                *outDataSize = sizeof(CFStringRef);
            } else if (sel == kAudioPlugInPropertyResourceBundle) {
                *((CFStringRef *)outData) = CFSTR("");
                *outDataSize = sizeof(CFStringRef);
            } else if (sel == kAudioObjectPropertyOwnedObjects || sel == kAudioPlugInPropertyDeviceList) {
                *((AudioObjectID *)outData) = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            } else if (sel == kAudioPlugInPropertyTranslateUIDToDevice) {
                if (inQualifierDataSize == sizeof(CFStringRef) && inQualifierData) {
                    CFStringRef uid = *((const CFStringRef *)inQualifierData);
                    *((AudioObjectID *)outData) = (uid && CFEqual(uid, CFSTR(kDevice_UID))) ? kObjectID_Device : kAudioObjectUnknown;
                } else {
                    *((AudioObjectID *)outData) = kAudioObjectUnknown;
                }
                *outDataSize = sizeof(AudioObjectID);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;

        case kObjectID_Device:
            if (sel == kAudioObjectPropertyBaseClass) {
                *((AudioClassID *)outData) = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyClass) {
                *((AudioClassID *)outData) = kAudioDeviceClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyOwner) {
                *((AudioObjectID *)outData) = kObjectID_PlugIn;
                *outDataSize = sizeof(AudioObjectID);
            } else if (sel == kAudioObjectPropertyName) {
                *((CFStringRef *)outData) = CFSTR("HDMI Volume");
                *outDataSize = sizeof(CFStringRef);
            } else if (sel == kAudioObjectPropertyManufacturer) {
                *((CFStringRef *)outData) = CFSTR("HDMI Volume");
                *outDataSize = sizeof(CFStringRef);
            } else if (sel == kAudioDevicePropertyDeviceUID) {
                *((CFStringRef *)outData) = CFSTR(kDevice_UID);
                *outDataSize = sizeof(CFStringRef);
            } else if (sel == kAudioDevicePropertyModelUID) {
                *((CFStringRef *)outData) = CFSTR(kDevice_ModelUID);
                *outDataSize = sizeof(CFStringRef);
            } else if (sel == kAudioDevicePropertyTransportType) {
                *((UInt32 *)outData) = kAudioDeviceTransportTypeVirtual;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioDevicePropertyRelatedDevices) {
                *((AudioObjectID *)outData) = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            } else if (sel == kAudioDevicePropertyClockDomain) {
                *((UInt32 *)outData) = 0;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioDevicePropertyDeviceIsAlive) {
                *((UInt32 *)outData) = 1;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioDevicePropertyDeviceIsRunning) {
                pthread_mutex_lock(&gState.state_mutex);
                *((UInt32 *)outData) = gState.io_client_count > 0 ? 1 : 0;
                pthread_mutex_unlock(&gState.state_mutex);
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioDevicePropertyDeviceCanBeDefaultDevice ||
                       sel == kAudioDevicePropertyDeviceCanBeDefaultSystemDevice) {
                *((UInt32 *)outData) = (scope == kAudioObjectPropertyScopeOutput) ? 1 : 0;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioDevicePropertyLatency || sel == kAudioDevicePropertySafetyOffset || sel == kAudioDevicePropertyIsHidden) {
                *((UInt32 *)outData) = 0;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioDevicePropertyZeroTimeStampPeriod) {
                *((UInt32 *)outData) = kZeroTimestampPeriod;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioObjectPropertyOwnedObjects) {
                AudioObjectID *ids = (AudioObjectID *)outData;
                ids[0] = kObjectID_Stream_Input;
                ids[1] = kObjectID_Stream_Output;
                ids[2] = kObjectID_Volume_Output_Master;
                ids[3] = kObjectID_Mute_Output_Master;
                *outDataSize = 4 * sizeof(AudioObjectID);
            } else if (sel == kAudioDevicePropertyStreams) {
                AudioObjectID *ids = (AudioObjectID *)outData;
                if (scope == kAudioObjectPropertyScopeInput) {
                    ids[0] = kObjectID_Stream_Input;
                    *outDataSize = sizeof(AudioObjectID);
                } else if (scope == kAudioObjectPropertyScopeOutput) {
                    ids[0] = kObjectID_Stream_Output;
                    *outDataSize = sizeof(AudioObjectID);
                } else {
                    ids[0] = kObjectID_Stream_Input;
                    ids[1] = kObjectID_Stream_Output;
                    *outDataSize = 2 * sizeof(AudioObjectID);
                }
            } else if (sel == kAudioObjectPropertyControlList) {
                AudioObjectID *ids = (AudioObjectID *)outData;
                ids[0] = kObjectID_Volume_Output_Master;
                ids[1] = kObjectID_Mute_Output_Master;
                *outDataSize = 2 * sizeof(AudioObjectID);
            } else if (sel == kAudioDevicePropertyNominalSampleRate) {
                pthread_mutex_lock(&gState.state_mutex);
                *((Float64 *)outData) = gState.sample_rate;
                pthread_mutex_unlock(&gState.state_mutex);
                *outDataSize = sizeof(Float64);
            } else if (sel == kAudioDevicePropertyAvailableNominalSampleRates) {
                AudioValueRange *ranges = (AudioValueRange *)outData;
                for (UInt32 i = 0; i < kNumSupportedSampleRates; ++i) {
                    ranges[i].mMinimum = kSupportedSampleRates[i];
                    ranges[i].mMaximum = kSupportedSampleRates[i];
                }
                *outDataSize = kNumSupportedSampleRates * sizeof(AudioValueRange);
            } else if (sel == kAudioDevicePropertyPreferredChannelsForStereo) {
                UInt32 *ch = (UInt32 *)outData;
                ch[0] = 1;
                ch[1] = 2;
                *outDataSize = 2 * sizeof(UInt32);
            } else if (sel == kAudioDevicePropertyPreferredChannelLayout) {
                AudioChannelLayout *layout = (AudioChannelLayout *)outData;
                layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                layout->mChannelBitmap = 0;
                layout->mNumberChannelDescriptions = 2;
                layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                layout->mChannelDescriptions[0].mChannelFlags = 0;
                layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
                layout->mChannelDescriptions[1].mChannelFlags = 0;
                *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + 2 * sizeof(AudioChannelDescription);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output: {
            bool is_input = (inObjectID == kObjectID_Stream_Input);
            if (sel == kAudioObjectPropertyBaseClass) {
                *((AudioClassID *)outData) = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyClass) {
                *((AudioClassID *)outData) = kAudioStreamClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyOwner) {
                *((AudioObjectID *)outData) = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            } else if (sel == kAudioStreamPropertyIsActive) {
                *((UInt32 *)outData) = 1;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioStreamPropertyDirection) {
                *((UInt32 *)outData) = is_input ? 1 : 0;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioStreamPropertyTerminalType) {
                *((UInt32 *)outData) = is_input ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeSpeaker;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioStreamPropertyStartingChannel) {
                *((UInt32 *)outData) = 1;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioStreamPropertyLatency) {
                *((UInt32 *)outData) = 0;
                *outDataSize = sizeof(UInt32);
            } else if (sel == kAudioStreamPropertyVirtualFormat || sel == kAudioStreamPropertyPhysicalFormat) {
                pthread_mutex_lock(&gState.state_mutex);
                fill_asbd((AudioStreamBasicDescription *)outData, gState.sample_rate);
                pthread_mutex_unlock(&gState.state_mutex);
                *outDataSize = sizeof(AudioStreamBasicDescription);
            } else if (sel == kAudioStreamPropertyAvailableVirtualFormats || sel == kAudioStreamPropertyAvailablePhysicalFormats) {
                AudioStreamRangedDescription *descs = (AudioStreamRangedDescription *)outData;
                for (UInt32 i = 0; i < kNumSupportedSampleRates; ++i) {
                    fill_asbd(&descs[i].mFormat, kSupportedSampleRates[i]);
                    descs[i].mSampleRateRange.mMinimum = kSupportedSampleRates[i];
                    descs[i].mSampleRateRange.mMaximum = kSupportedSampleRates[i];
                }
                *outDataSize = kNumSupportedSampleRates * sizeof(AudioStreamRangedDescription);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;
        }

        case kObjectID_Volume_Output_Master:
            if (sel == kAudioObjectPropertyBaseClass) {
                *((AudioClassID *)outData) = kAudioControlClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyClass) {
                *((AudioClassID *)outData) = kAudioVolumeControlClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyOwner) {
                *((AudioObjectID *)outData) = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            } else if (sel == kAudioControlPropertyScope) {
                *((AudioObjectPropertyScope *)outData) = kAudioObjectPropertyScopeOutput;
                *outDataSize = sizeof(AudioObjectPropertyScope);
            } else if (sel == kAudioControlPropertyElement) {
                *((AudioObjectPropertyElement *)outData) = kAudioObjectPropertyElementMain;
                *outDataSize = sizeof(AudioObjectPropertyElement);
            } else if (sel == kAudioLevelControlPropertyScalarValue) {
                *((Float32 *)outData) = atomic_load_explicit(&gState.volume_scalar, memory_order_relaxed);
                *outDataSize = sizeof(Float32);
            } else if (sel == kAudioLevelControlPropertyDecibelValue) {
                Float32 s = atomic_load_explicit(&gState.volume_scalar, memory_order_relaxed);
                *((Float32 *)outData) = scalar_to_db(s);
                *outDataSize = sizeof(Float32);
            } else if (sel == kAudioLevelControlPropertyDecibelRange) {
                AudioValueRange *r = (AudioValueRange *)outData;
                r->mMinimum = -60.0;
                r->mMaximum = 0.0;
                *outDataSize = sizeof(AudioValueRange);
            } else if (sel == kAudioLevelControlPropertyConvertScalarToDecibels) {
                Float32 s = *((Float32 *)outData);
                *((Float32 *)outData) = scalar_to_db(s);
                *outDataSize = sizeof(Float32);
            } else if (sel == kAudioLevelControlPropertyConvertDecibelsToScalar) {
                Float32 db = *((Float32 *)outData);
                *((Float32 *)outData) = db_to_scalar(db);
                *outDataSize = sizeof(Float32);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;

        case kObjectID_Mute_Output_Master:
            if (sel == kAudioObjectPropertyBaseClass) {
                *((AudioClassID *)outData) = kAudioControlClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyClass) {
                *((AudioClassID *)outData) = kAudioMuteControlClassID;
                *outDataSize = sizeof(AudioClassID);
            } else if (sel == kAudioObjectPropertyOwner) {
                *((AudioObjectID *)outData) = kObjectID_Device;
                *outDataSize = sizeof(AudioObjectID);
            } else if (sel == kAudioControlPropertyScope) {
                *((AudioObjectPropertyScope *)outData) = kAudioObjectPropertyScopeOutput;
                *outDataSize = sizeof(AudioObjectPropertyScope);
            } else if (sel == kAudioControlPropertyElement) {
                *((AudioObjectPropertyElement *)outData) = kAudioObjectPropertyElementMain;
                *outDataSize = sizeof(AudioObjectPropertyElement);
            } else if (sel == kAudioBooleanControlPropertyValue) {
                *((UInt32 *)outData) = atomic_load_explicit(&gState.muted, memory_order_relaxed) ? 1 : 0;
                *outDataSize = sizeof(UInt32);
            } else {
                return kAudioHardwareUnknownPropertyError;
            }
            return kAudioHardwareNoError;

        default:
            (void)inDataSize;
            return kAudioHardwareBadObjectError;
    }
}

static OSStatus HDV_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData) {
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (!inAddress || !inData) return kAudioHardwareIllegalOperationError;

    if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate && inDataSize >= sizeof(Float64)) {
        Float64 req_rate = *((const Float64 *)inData);
        for (UInt32 i = 0; i < kNumSupportedSampleRates; ++i) {
            if (kSupportedSampleRates[i] == req_rate) {
                if (gState.host && gState.host->RequestDeviceConfigurationChange) {
                    gState.host->RequestDeviceConfigurationChange(gState.host, kObjectID_Device, (UInt64)req_rate, NULL);
                }
                return kAudioHardwareNoError;
            }
        }
        return kAudioDeviceUnsupportedFormatError;
    }

    if (inObjectID == kObjectID_Volume_Output_Master && inDataSize >= sizeof(Float32)) {
        Float32 scalar = 0.0f;
        if (inAddress->mSelector == kAudioLevelControlPropertyScalarValue) {
            scalar = *((const Float32 *)inData);
        } else if (inAddress->mSelector == kAudioLevelControlPropertyDecibelValue) {
            scalar = db_to_scalar(*((const Float32 *)inData));
        } else {
            return kAudioHardwareUnknownPropertyError;
        }
        if (scalar < 0.0f) scalar = 0.0f;
        if (scalar > 1.0f) scalar = 1.0f;
        atomic_store_explicit(&gState.volume_scalar, scalar, memory_order_relaxed);
        notify_host_property_changed(kObjectID_Volume_Output_Master, kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeOutput);
        notify_host_property_changed(kObjectID_Volume_Output_Master, kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeOutput);
        return kAudioHardwareNoError;
    }

    if (inObjectID == kObjectID_Mute_Output_Master && inAddress->mSelector == kAudioBooleanControlPropertyValue && inDataSize >= sizeof(UInt32)) {
        bool m = (*((const UInt32 *)inData)) != 0;
        atomic_store_explicit(&gState.muted, m, memory_order_relaxed);
        notify_host_property_changed(kObjectID_Mute_Output_Master, kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeOutput);
        return kAudioHardwareNoError;
    }

    return kAudioHardwareUnknownPropertyError;
}

static OSStatus HDV_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    pthread_mutex_lock(&gState.state_mutex);
    if (gState.io_client_count == 0) {
        gState.anchor_host_time = mach_absolute_time();
        hdv_ring_reset(&gState.ring);
    }
    gState.io_client_count += 1;
    pthread_mutex_unlock(&gState.state_mutex);
    return kAudioHardwareNoError;
}

static OSStatus HDV_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    pthread_mutex_lock(&gState.state_mutex);
    if (gState.io_client_count > 0) {
        gState.io_client_count -= 1;
    }
    pthread_mutex_unlock(&gState.state_mutex);
    return kAudioHardwareNoError;
}

static OSStatus HDV_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    if (!outSampleTime || !outHostTime || !outSeed) return kAudioHardwareIllegalOperationError;

    UInt64 now = mach_absolute_time();
    Float64 ticks_per_period = gState.host_ticks_per_frame * (Float64)kZeroTimestampPeriod;
    UInt64 elapsed = (now >= gState.anchor_host_time) ? (now - gState.anchor_host_time) : 0;
    UInt64 periods = (UInt64)((Float64)elapsed / ticks_per_period);

    *outSampleTime = (Float64)(periods * kZeroTimestampPeriod);
    *outHostTime = gState.anchor_host_time + (UInt64)((Float64)periods * ticks_per_period);
    *outSeed = 1;
    return kAudioHardwareNoError;
}

static OSStatus HDV_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    if (!outWillDo || !outWillDoInPlace) return kAudioHardwareIllegalOperationError;
    bool will_do = (inOperationID == kAudioServerPlugInIOOperationWriteMix ||
                    inOperationID == kAudioServerPlugInIOOperationReadInput);
    *outWillDo = will_do;
    *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus HDV_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}

static OSStatus HDV_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer) {
    (void)inDriver; (void)inDeviceObjectID; (void)inStreamObjectID; (void)inClientID; (void)inIOCycleInfo; (void)ioSecondaryBuffer;
    if (!ioMainBuffer) return kAudioHardwareNoError;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        float scalar = atomic_load_explicit(&gState.volume_scalar, memory_order_relaxed);
        bool muted = atomic_load_explicit(&gState.muted, memory_order_relaxed);
        hdv_write_and_scale(&gState.ring, (const float *)ioMainBuffer, inIOBufferFrameSize, HDV_CHANNELS, scalar, muted);
    } else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        hdv_read_loopback(&gState.ring, (float *)ioMainBuffer, inIOBufferFrameSize, HDV_CHANNELS);
    }
    return kAudioHardwareNoError;
}

static OSStatus HDV_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}

static AudioServerPlugInDriverInterface gDriverInterface = {
    NULL,
    HDV_QueryInterface,
    HDV_AddRef,
    HDV_Release,
    HDV_Initialize,
    HDV_CreateDevice,
    HDV_DestroyDevice,
    HDV_AddDeviceClient,
    HDV_RemoveDeviceClient,
    HDV_PerformDeviceConfigurationChange,
    HDV_AbortDeviceConfigurationChange,
    HDV_HasProperty,
    HDV_IsPropertySettable,
    HDV_GetPropertyDataSize,
    HDV_GetPropertyData,
    HDV_SetPropertyData,
    HDV_StartIO,
    HDV_StopIO,
    HDV_GetZeroTimeStamp,
    HDV_WillDoIOOperation,
    HDV_BeginIOOperation,
    HDV_DoIOOperation,
    HDV_EndIOOperation
};

static AudioServerPlugInDriverInterface *gDriverInterfacePtr = &gDriverInterface;

void *HDMIVolumeCreate(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    (void)inAllocator;
    if (!CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        return NULL;
    }
    if (!gStateInitialized) {
        gState.interface = (AudioServerPlugInDriverInterface *)&gDriverInterface;
        atomic_init(&gState.ref_count, 1);
        gState.host = NULL;
        pthread_mutex_init(&gState.state_mutex, NULL);
        gState.sample_rate = 48000.0;
        gState.io_client_count = 0;
        gState.anchor_host_time = 0;
        update_host_ticks_per_frame();
        atomic_init(&gState.volume_scalar, 0.75f);
        atomic_init(&gState.muted, false);
        hdv_ring_init(&gState.ring);
        gStateInitialized = true;
    } else {
        atomic_fetch_add(&gState.ref_count, 1);
    }
    return &gDriverInterfacePtr;
}
