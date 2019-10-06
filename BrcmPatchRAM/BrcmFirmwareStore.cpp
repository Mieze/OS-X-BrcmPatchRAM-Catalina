/*
 *  Released under "The GNU General Public License (GPL-2.0)"
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *  Zlib implementation based on /apple/xnu/libkern/c++/OSKext.cpp
 */

#include "Common.h"
#include "BrcmFirmwareStore.h"
#ifdef FIRMWAREDATA
#include "FirmwareData.h"
#endif

#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>

/***************************************
 * Zlib Decompression
 ***************************************/
#include <libkern/zlib.h>

extern "C"
{
    static void* z_alloc(void*, u_int items, u_int size);
    static void z_free(void*, void *ptr);
    
    typedef struct z_mem
    {
        UInt32 alloc_size;
        UInt8 data[0];
    } z_mem;
    
    /*
     * Space allocation and freeing routines for use by zlib routines.
     */
    void* z_alloc(void* notused __unused, u_int num_items, u_int size)
    {
        void* result = NULL;
        z_mem* zmem = NULL;
        UInt32 total = num_items * size;
        UInt32 allocSize =  total + sizeof(zmem);
        
        zmem = (z_mem*)IOMalloc(allocSize);
        
        if (zmem)
        {
            zmem->alloc_size = allocSize;
            result = (void*)&(zmem->data);
        }
        
        return result;
    }
    
    void z_free(void* notused __unused, void* ptr)
    {
        UInt32* skipper = (UInt32 *)ptr - 1;
        z_mem* zmem = (z_mem*)skipper;
        IOFree((void*)zmem, zmem->alloc_size);
    }
};

/*
 * Decompress the firmware using zlib inflate (If not compressed, return data normally)
 */
OSData* BrcmFirmwareStore::decompressFirmware(OSData* firmware)
{
    z_stream zstream;
    int zlib_result;
    int bufferSize = 0;
    void* buffer = NULL;
    OSData* result = NULL;
    
    // Verify if the data is compressed
    UInt16* magic = (UInt16*)firmware->getBytesNoCopy();
    
    if (*magic != 0x0178     // Zlib no compression
        && *magic != 0x9c78  // Zlib default compression
        && *magic != 0xda78) // Zlib maximum compression
    {
        // Return the data as-is
        firmware->retain();
        return firmware;
    }
    
    bufferSize = firmware->getLength() * 4;
    
    buffer = IOMalloc(bufferSize);
    
    bzero(&zstream, sizeof(zstream));
    
    zstream.next_in   = (unsigned char*)firmware->getBytesNoCopy();
    zstream.avail_in  = firmware->getLength();
    
    zstream.next_out  = (unsigned char*)buffer;
    zstream.avail_out = bufferSize;
    
    zstream.zalloc    = z_alloc;
    zstream.zfree     = z_free;
    
    zlib_result = inflateInit(&zstream);
    
    if (zlib_result != Z_OK)
    {
        IOFree(buffer, bufferSize);
        return NULL;
    }
    
    zlib_result = inflate(&zstream, Z_FINISH);
    
    if (zlib_result == Z_STREAM_END || zlib_result == Z_OK)
        // Allocate final result
        result = OSData::withBytes(buffer, (unsigned int)zstream.total_out);
    
    inflateEnd(&zstream);
    IOFree(buffer, bufferSize);
    
    return result;
}

/**********************************************
 * IntelHex firmware parsing
 **********************************************/
#define HEX_LINE_PREFIX ':'
#define HEX_HEADER_SIZE 4

#define REC_TYPE_DATA 0 // Data
#define REC_TYPE_EOF 1  // End of File
#define REC_TYPE_ESA 2  // Extended Segment Address
#define REC_TYPE_SSA 3  // Start Segment Address
#define REC_TYPE_ELA 4  // Extended Linear Address
#define REC_TYPE_SLA 5  // Start Linear Address

/*
 * Validate if the current character is a valid hexadecimal character
 */
static inline bool validHexChar(UInt8 hex)
{
    return (hex >= 'a' && hex <= 'f') || (hex >= 'A' && hex <= 'F') || (hex >= '0' && hex <= '9');
}

/*
 * Convert char '0-9,A-F' to hexadecimal values
 */
static inline void hex_nibble(UInt8 hex, UInt8 &output)
{
    output <<= 4;
    
    if (hex >= 'a')
        output |= (0x0A + (hex - 'a')) & 0x0F;
    if (hex >= 'A')
        output |= (0x0A + (hex - 'A')) & 0x0F;
    else
        output |= (hex - '0') & 0x0F;
}

/*
 * Two's complement checksum
 */
static char check_sum(const UInt8* data, UInt16 len)
{
    UInt32 crc = 0;
    
    for (int i = 0; i < len; i++)
        crc += *(data + i);
    
    return (~crc + 1) & 0xFF;
}

OSArray* BrcmFirmwareStore::parseFirmware(OSData* firmwareData)
{
    // Vendor Specific: Launch RAM
    UInt8 HCI_VSC_LAUNCH_RAM[] = { 0x4c, 0xfc };
    
    OSArray* instructions = OSArray::withCapacity(1);
    if (!instructions)
        return NULL;

    UInt8* data = (UInt8*)firmwareData->getBytesNoCopy();
    UInt32 address = 0;
    UInt8 binary[0x110];
    
    if (*data != HEX_LINE_PREFIX)
    {
        DebugLog("parseFirmware - Invalid firmware data.\n");
        goto exit_error;
    }
    
    while (*data == HEX_LINE_PREFIX)
    {
        bzero(binary, sizeof(binary));
        data++;
        
        int offset = 0;
        
        // Read all hex characters for this line
        while (validHexChar(*data))
        {
            hex_nibble(*data++, binary[offset]);
            hex_nibble(*data++, binary[offset++]);
        }
        
        // Parse line data
        UInt8 length = binary[0];
        UInt16 addr = binary[1] << 8 | binary[2];
        UInt8 record_type = binary[3];
        UInt8 checksum = binary[HEX_HEADER_SIZE + length];
        
        UInt8 calc_checksum = check_sum(binary, HEX_HEADER_SIZE  + length);
        
        if (checksum != calc_checksum)
        {
            DebugLog("parseFirmware - Invalid firmware, checksum mismatch.\n");
            goto exit_error;
        }
        
        // ParseFirmware class only supports I32HEX format
        switch (record_type)
        {
            // Data
            case REC_TYPE_DATA:
            {
                address = (address & 0xFFFF0000) | addr;
                
                // Reserved 4 bytes for the address
                length += 4;
                
                // Allocate instruction (Opcode - 2 bytes, length - 1 byte)
                OSData* instruction = OSData::withCapacity(3 + length);
                if (!instruction)
                    goto exit_error;
                
                instruction->appendBytes(HCI_VSC_LAUNCH_RAM, sizeof(HCI_VSC_LAUNCH_RAM));
                instruction->appendBytes(&length, sizeof(length));
                instruction->appendBytes(&address, sizeof(address));
                instruction->appendBytes(&binary[4], length - 4);
                
                instructions->setObject(instruction);
                instruction->release();
                break;
            }
            // End of File
            case REC_TYPE_EOF:
                return instructions;
            // Extended Segment Address
            case REC_TYPE_ESA:
                // Segment address multiplied by 16
                address = binary[4] << 8 | binary[5];
                address <<= 4;
                break;
                // Start Segment Address
            case REC_TYPE_SSA:
                // Set CS:IP register for 80x86
                DebugLog("parseFirmware - Invalid firmware, unsupported start segment address instruction.\n");
                goto exit_error;
                // Extended Linear Address
            case REC_TYPE_ELA:
                // Set new higher 16 bits of the current address
                address = binary[4] << 24 | binary[5] << 16;
                break;
                // Start Linear Address
            case REC_TYPE_SLA:
                // Set EIP of 80386 and higher
                DebugLog("parseFirmware - Invalid firmware, unsupported start linear address instruction.\n");
                goto exit_error;
            default:
                DebugLog("parseFirmware - Invalid firmware, unknown record type encountered: 0x%02x.\n", record_type);
                goto exit_error;
        }
        
        // Skip over any trailing newlines / whitespace
        while (!validHexChar(*data) && !(*data == HEX_LINE_PREFIX))
            data++;
    }
    
    DebugLog("parseFirmware - Invalid firmware.\n");
    
exit_error:
    OSSafeReleaseNULL(instructions);
    return NULL;
}

OSDefineMetaClassAndStructors(BrcmFirmwareStore, IOService)

bool BrcmFirmwareStore::start(IOService *provider)
{
    DebugLog("Firmware store start\n");
    
    if (!super::start(provider))
        return false;
    
    // place version/build info in ioreg properties RM,Build and RM,Version
    char buf[128];
    snprintf(buf, sizeof(buf), "%s %s", OSKextGetCurrentIdentifier(), OSKextGetCurrentVersionString());
    setProperty("RM,Version", buf);
#ifdef DEBUG
    setProperty("RM,Build", "Debug-" LOGNAME);
#else
    setProperty("RM,Build", "Release-" LOGNAME);
#endif

    mFirmwares = OSDictionary::withCapacity(1);
    if (!mFirmwares)
        return false;
    
    mCompletionLock = IOLockAlloc();
    if (!mCompletionLock)
        return false;

    mDataLock = IOLockAlloc();
    if (!mDataLock)
        return false;

    registerService();

    return true;
}

void BrcmFirmwareStore::stop(IOService *provider)
{
    DebugLog("Firmware store stop\n");
    
    OSSafeReleaseNULL(mFirmwares);
    
    if (mCompletionLock)
    {
        IOLockFree(mCompletionLock);
        mCompletionLock = NULL;
    }

    if (mDataLock)
    {
        IOLockFree(mDataLock);
        mDataLock = NULL;
    }
    
    super::stop(provider);
}

void BrcmFirmwareStore::requestResourceCallback(OSKextRequestTag requestTag, OSReturn result, const void * resourceData, uint32_t resourceDataLength, void* context1)
{
    ResourceCallbackContext *context = (ResourceCallbackContext*)context1;
    
    IOLockLock(context->me->mCompletionLock);

    if (kOSReturnSuccess == result)
    {
        DebugLog("OSKextRequestResource Callback: %d bytes of data.\n", resourceDataLength);
        
        context->firmware = OSData::withBytes(resourceData, resourceDataLength);
    }
    else
        DebugLog("OSKextRequestResource Callback: %08x.\n", result);
    
    IOLockUnlock(context->me->mCompletionLock);
    
    // wake waiting task in performUpgrade (in IOLockSleep)...
    IOLockWakeup(context->me->mCompletionLock, context->me, true);
}

OSData* BrcmFirmwareStore::loadFirmwareFile(const char* filename, const char* suffix)
{
    IOLockLock(mCompletionLock);

    ResourceCallbackContext context = { .me = this, .firmware = NULL };

    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "%s.%s", filename, suffix);
    
#ifdef DEBUG
    OSReturn ret = OSKextRequestResource(OSKextGetCurrentIdentifier(),
                          path,
                          requestResourceCallback,
                          &context,
                          NULL);
    
    AlwaysLog("OSKextRequestResource: %08x\n", ret);
#else
    OSKextRequestResource(OSKextGetCurrentIdentifier(),
                          path,
                          requestResourceCallback,
                          &context,
                          NULL);
#endif
    // wait for completion of the async read
    IOLockSleep(mCompletionLock, this, 0);
    
    IOLockUnlock(mCompletionLock);
    
    if (context.firmware)
    {
        AlwaysLog("Loaded firmware \"%s\" from resources.\n", path);
        return context.firmware;
    }

    return NULL;
}

OSData* BrcmFirmwareStore::loadFirmwareFiles(UInt16 vendorId, UInt16 productId, OSString* firmwareKey)
{
    char filename[PATH_MAX];
    snprintf(filename, PATH_MAX, "%04x_%04x", vendorId, productId);

    OSData* result = loadFirmwareFile(filename, kBrcmFirmwareCompressed);

    if (!result)
        result = loadFirmwareFile(filename, kBrmcmFirwareUncompressed);

    if (!result)
        result = loadFirmwareFile(firmwareKey->getCStringNoCopy(), kBrcmFirmwareCompressed);
    
    if (!result)
        result = loadFirmwareFile(firmwareKey->getCStringNoCopy(), kBrmcmFirwareUncompressed);

    return result;
}

OSArray* BrcmFirmwareStore::loadFirmware(UInt16 vendorId, UInt16 productId, OSString* firmwareKey)
{
    DebugLog("loadFirmware\n");
    
    // First try to load firmware from disk
    OSData* configuredData = loadFirmwareFiles(vendorId, productId, firmwareKey);

#ifdef FIRMWAREDATA
    char filename[PATH_MAX];
    // Next try to load from internal binary data
    if (!configuredData)
    {
        snprintf(filename, PATH_MAX, "%s.%s", firmwareKey->getCStringNoCopy(), kBrcmFirmwareCompressed);
        configuredData = lookupFirmware(filename);
        if (configuredData)
            AlwaysLog("Loaded compressed embedded firmware for key \"%s\".\n", firmwareKey->getCStringNoCopy());
    }
    if (!configuredData)
    {
        snprintf(filename, PATH_MAX, "%s.%s", firmwareKey->getCStringNoCopy(), kBrmcmFirwareUncompressed);
        configuredData = lookupFirmware(filename);
        if (configuredData)
            AlwaysLog("Loaded compressed embedded firmware for key \"%s\".\n", firmwareKey->getCStringNoCopy());
    }
#endif

    // Next try to load firmware from configuration
    if (!configuredData)
    {
        OSDictionary* firmwares = OSDynamicCast(OSDictionary, this->getProperty("Firmwares"));
    
        if (!firmwares)
        {
            AlwaysLog("Unable to locate BrcmFirmwareStore configured firmwares.\n");
            return NULL;
        }
    
        configuredData = OSDynamicCast(OSData, firmwares->getObject(firmwareKey));
        
        if (configuredData)
        {
            configuredData->retain();
            AlwaysLog("Retrieved firmware \"%s\" from internal configuration.\n", firmwareKey->getCStringNoCopy());
        }
    }
        
    if (!configuredData)
    {
        AlwaysLog("No firmware available for firmware key \"%s\".\n", firmwareKey->getCStringNoCopy());
        return NULL;
    }
    
    OSData* firmwareData = decompressFirmware(configuredData);
    
    if (!firmwareData)
    {
        configuredData->release();
        AlwaysLog("Failed to decompress firmware.\n");
        return NULL;
    }
    
    if (configuredData->getLength() < firmwareData->getLength())
        AlwaysLog("Decompressed firmware (%d bytes --> %d bytes).\n", configuredData->getLength(), firmwareData->getLength());
    else
        AlwaysLog("Non-compressed firmware.\n");

    configuredData->release();
    
    OSArray* instructions = parseFirmware(firmwareData);
    firmwareData->release();
    
    if (!instructions)
    {
        AlwaysLog("Firmware is not valid IntelHex firmware.\n");
        return NULL;
    }
    
    AlwaysLog("Firmware is valid IntelHex firmware.\n");
    
    return instructions;
}

OSArray* BrcmFirmwareStore::getFirmware(UInt16 vendorId, UInt16 productId, OSString* firmwareKey)
{
    DebugLog("getFirmware\n");
    
    if (!firmwareKey || firmwareKey->getLength() == 0)
    {
        AlwaysLog("Current device has no FirmwareKey configured.\n");
        return NULL;
    }
    
    IOLockLock(mDataLock);
    OSArray* instructions = OSDynamicCast(OSArray, mFirmwares->getObject(firmwareKey));
    
    // Cached instructions found for firmwareKey?
    if (!instructions)
    {
        // Load instructions for firmwareKey
        instructions = loadFirmware(vendorId, productId, firmwareKey);
        
        // Add instructions to the firmwares cache
        if (instructions)
        {
            mFirmwares->setObject(firmwareKey, instructions);
            instructions->release();
        }
    }
    else
        DebugLog("Retrieved cached firmware for \"%s\".\n", firmwareKey->getCStringNoCopy());

    IOLockUnlock(mDataLock);
    
    return instructions;
}


