//======================================================================
//
// CELoader - A simple UEFI loader for Windows CE.
//
// Based on code from the EFI Toolkit, some of which is
// Copyright (c) 1998  Intel Corporation
//
// Module Name:
//
//    celoader.c
//    
// Author:
//    Douglas Boling
//
//======================================================================

// Comment this out if you don't want the loader to jump
// to the loaded NK.BIN image. Good for testing.
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PeCoffLib.h>
#include <Library/PrintLib.h>

#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePathToText.h>

#include <Guid/Acpi.h>

#include "bootarg.h"

#define memset(dest, ch, count) SetMem(dest,(UINTN)(count),(UINT8)(ch))

BOOT_ARGS *pBootArgs;

// Necessary for video protocol support
#include "GraphicsOutput.h"

#if 0
EFI_GUID  gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID_STRING(&gEfiGraphicsOutputProtocolGuid, "EFI Graphics Output Protocol", "UEFI Graphics Output Protocol");
#endif

EFI_HANDLE           g_ImageHandle;
EFI_SYSTEM_TABLE     *g_pSystemTable;

CHAR16 *
DevicePathToStr (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevPath
  )
{
  CHAR16  *Text;

  Text = ConvertDevicePathToText (
           DevPath,
           FALSE,
           TRUE
           );
  if (Text == NULL) {
    Text = AllocateCopyPool (sizeof (L"?"), L"?");
    ASSERT (Text != NULL);
  }

  return Text;
}

typedef void (*PFN_LAUNCH)();
EFI_STATUS FindAcpiTable (IN EFI_SYSTEM_TABLE *pSystemTable, UINTN *pdwRSDPPtr);
EFI_STATUS EFIAPI DisplaySettings (IN UINTN HandleIdx, IN BOOLEAN HandleValid);
void DumpMemoryMap (IN EFI_HANDLE ImageHandle);
void DumpVidModes ();
void SetBootArgs(UINT32 dwDefVideoWidth, UINT32 dwDefVideoHeight, 
				 UINT32 dwDefVideoDepth, UINT32 dwStride, UINT32 pVideoBuff);
//======================================================================
// Bootloader Entry Point
//======================================================================
EFI_STATUS LoaderMain (IN EFI_HANDLE ImageHandle, 
					   IN EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS              Status;

    EFI_TIME                Time;

    CHAR16                  *DevicePathAsString;

    EFI_LOADED_IMAGE        *LoadedImage;
    EFI_DEVICE_PATH         *DevicePath;

    EFI_FILE_IO_INTERFACE   *Vol;
    EFI_FILE_HANDLE         RootFs;
    EFI_FILE_HANDLE         CurDir;
    EFI_FILE_HANDLE         FileHandle;
    CHAR16                  FileName[100];
    UINTN                   i = 0;

    UINTN                   Size = 0;
    VOID                    *OsKernelBuffer = NULL;


	UINTN 					dwNKStart = 0;
	UINTN 					dwNKLength = 0;
	UINTN 					dwEntryOffset = 0;

	UINTN 					dwCount = 0;
	UINTN 					dwBlkStart = 0;
	UINTN 					dwBlkLength = 0;
	UINT32					dwBlkChecksum = 0;
	CHAR8           bHdr[8];
  UINTN           MapKey = 0;
	UINTN           dwACPI = 0;

    //
    // Print a message to the console output device.
    //
    Print(L"OS Loader application started\n");

    //
    // Print Date and Time 
    //
    Status = gRT->GetTime(&Time,NULL);

    if (!EFI_ERROR(Status)) 
	{
        Print(L"Date : %02d/%02d/%04d  Time : %02d:%02d:%02d\n",
		      Time.Month,Time.Day,Time.Year,Time.Hour,Time.Minute,Time.Second);
    }

#ifdef DUMP_INFO
	DumpMemoryMap (ImageHandle);
#endif //DUMP_INFO

    //
    // Get the device handle and file path to the EFI OS Loader itself.
    //
    Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid,
                                 (VOID*)&LoadedImage);

    if (EFI_ERROR(Status)) 
	{
        Print(L"Can not retrieve a LoadedImageProtocol handle for ImageHandle\n");
        gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
    }

    Status = gBS->HandleProtocol (LoadedImage->DeviceHandle, &gEfiDevicePathProtocolGuid,
                                 (VOID*)&DevicePath);

    if (EFI_ERROR(Status) || DevicePath==NULL) 
	{
        Print(L"Can not find a DevicePath handle for LoadedImage->DeviceHandle\n");
        gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
    }

    DevicePathAsString = DevicePathToStr(DevicePath);
    if (DevicePathAsString != NULL)
	{
        Print (L"Image device : %s\n", DevicePathAsString);
        FreePool(DevicePathAsString);
    }

    DevicePathAsString = DevicePathToStr(LoadedImage->FilePath);
    if (DevicePathAsString != NULL)
	{
        Print (L"Image file   : %s\n", DevicePathToStr (LoadedImage->FilePath));
        FreePool(DevicePathAsString);
    }

    Print (L"Image Base   : %X\n", LoadedImage->ImageBase);
    Print (L"Image Size   : %X\n", LoadedImage->ImageSize);

	//
	// Get ACPI Table Ptr
	//
	Status = FindAcpiTable (gST, &dwACPI);
	Print (L"FindAcpiTable returned status %x,  ptr:%x\r\n", Status, dwACPI);

    //
    // Open the volume for the device where the Loader was loaded from.
    //
    Status = gBS->HandleProtocol (LoadedImage->DeviceHandle,&gEfiSimpleFileSystemProtocolGuid,
                                 (VOID*)&Vol);
    if (EFI_ERROR(Status)) 
	{
        Print(L"Can not get a FileSystem handle for LoadedImage->DeviceHandle\n");
        gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
    }

    Status = Vol->OpenVolume (Vol, &RootFs);
    if (EFI_ERROR(Status)) 
	{
        Print(L"Can not open the volume for the file system\n");
        gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
    }

    CurDir = RootFs;

    //
    // Open the file NK.BIN in the same path as the EFI OS Loader.
    //
    DevicePathAsString = DevicePathToStr(LoadedImage->FilePath);
    if (DevicePathAsString!=NULL)
	{
        StrCpyS(FileName, sizeof(FileName), DevicePathAsString);
        FreePool(DevicePathAsString);
    }
  // Find the end of the directory portion of the path.
    for(i=StrLen(FileName); (i > 0) && (FileName[i] != '/'); i--);
    if(FileName[i-1] == '\\' )
    {
      i--;
    }

    FileName[i] = 0;
    StrCatS(FileName, sizeof(FileName), L"\\NK.BIN");

    // Open NK file.
    Status = CurDir->Open (CurDir, &FileHandle, FileName,
                           EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status) == 0) 
	{
		Print(L"Opened %s\n",FileName);

		// 
		// Read header
		//
		Size = 7;
		Status = FileHandle->Read(FileHandle, &Size, &bHdr);
		if (EFI_ERROR(Status)) 
		{
			Print(L"0. Can not read the file %s\n",FileName);
			gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
		}
		//Print(L"Hdr = >%S<\n",bHdr);

		Size = 4;
		Status = FileHandle->Read(FileHandle, &Size, &dwNKStart);

		if (EFI_ERROR(Status)) 
		{
			Print(L"1. Can not read the file %s\n",FileName);
			gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
		}

		Size = 4;
		Status = FileHandle->Read(FileHandle, &Size, &dwNKLength);
		if (EFI_ERROR(Status)) 
		{
			Print(L"2. Can not read the file %s\n",FileName);
			gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
		}
		Print(L"NkStart: %X  NkLen: %X\n", dwNKStart, dwNKLength);


		//
		// Allocate a buffer for NK.BIN
		//
		Size = dwNKLength;
		OsKernelBuffer = (void *)(dwNKStart & 0x7fffffff);
		gBS->AllocatePool(EfiLoaderData, Size, &OsKernelBuffer);

		if (OsKernelBuffer == NULL) 
		{
			Print(L"Can not allocate a buffer for the file %s\n",FileName);
			gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
		}
		else
			Print(L"buffer allocated\n");
			
	
		dwCount = 0;
		//while (dwCount < 5)
		while (1)
		{
			Size = 4;
			Status = FileHandle->Read(FileHandle, &Size, &dwBlkStart);
			if (EFI_ERROR(Status)) 
			{
				Print(L"Can not read dwBlkStart\n");
				gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
			}

			Size = 4;
			Status = FileHandle->Read(FileHandle, &Size, &dwBlkLength);
			if (EFI_ERROR(Status)) 
			{
				Print(L"Can not read blk size\n");
				gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
			}
			Size = 4;
			Status = FileHandle->Read(FileHandle, &Size, &dwBlkChecksum);
			if (EFI_ERROR(Status)) 
			{
				Print(L"Can not blk check\n");
				gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
			}

			if ((dwBlkStart == 0) && (dwBlkChecksum == 0))
				break;

			Print(L"Block start %x Len %x check %x\n",dwBlkStart, dwBlkLength, dwBlkChecksum);

			// Read the block data  
			Size = dwBlkLength;
			Status = FileHandle->Read(FileHandle, &Size, (void *)(dwBlkStart & 0x7fffffff));
			if (EFI_ERROR(Status)) 
			{
				Print(L"Can not blk size\n");
				gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
			}
			dwCount++;
		}
		Print(L"Block read complete. %d blocks\n", dwCount);

		dwEntryOffset = dwBlkLength;

		Print(L"NK entry %x.\n", dwEntryOffset);

		//
		// Close NK.BIN 
		//

		Status = FileHandle->Close(FileHandle);
		if (EFI_ERROR(Status)) 
		{
			Print(L"Can not close the file %s\n",FileName);
			gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
		}

		//
		// Free the resources allocated from pool.
		//
	}
	else
	{
        Print(L"Can not open the file %s\n",FileName);
        gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
    }

	// Dump the available video modes and set mode 0
	DumpVidModes ();

    //
    // Transition from Boot Services to Run Time Services.  
    //
    Print(L"Call ExitBootServices()\n");

    gBS->ExitBootServices(ImageHandle,MapKey);

	// Call into the OS
    ((PFN_LAUNCH)(dwEntryOffset))();

    return EFI_SUCCESS;
}

void PrintGUID (EFI_GUID guid)
{
	Print (L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	       guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

BOOLEAN
GrowBuffer(
    IN OUT EFI_STATUS   *Status,
    IN OUT VOID         **Buffer,
    IN UINTN            BufferSize
    )
/*++

Routine Description:

    Helper function called as part of the code needed
    to allocate the proper sized buffer for various
    EFI interfaces.

Arguments:

    Status      - Current status

    Buffer      - Current allocated buffer, or NULL

    BufferSize  - Current buffer size needed
    
Returns:
    
    TRUE - if the buffer was reallocated and the caller
    should try the API again.

--*/
{
    BOOLEAN         TryAgain;

    //
    // If this is an initial request, buffer will be null with a new buffer size
    //

    if (!*Buffer && BufferSize) {
        *Status = EFI_BUFFER_TOO_SMALL;
    }

    //
    // If the status code is "buffer too small", resize the buffer
    //
        
    TryAgain = FALSE;
    if (*Status == EFI_BUFFER_TOO_SMALL) {

        if (*Buffer) {
            FreePool (*Buffer);
        }

        *Buffer = AllocatePool (BufferSize);

        if (*Buffer) {
            TryAgain = TRUE;
        } else {
            *Status = EFI_OUT_OF_RESOURCES;
        }
    }

    //
    // If there's an error, free the buffer
    //

    if (!TryAgain && EFI_ERROR(*Status) && *Buffer) {
        FreePool (*Buffer);
        *Buffer = NULL;
    }

    return TryAgain;
}

EFI_MEMORY_DESCRIPTOR *
LibMemoryMap (
    OUT UINTN               *NoEntries,
    OUT UINTN               *MapKey,
    OUT UINTN               *DescriptorSize,
    OUT UINT32              *DescriptorVersion
    )
{
    EFI_STATUS              Status;
    EFI_MEMORY_DESCRIPTOR   *Buffer;
    UINTN                   BufferSize;

    //
    // Initialize for GrowBuffer loop
    //

    Status = EFI_SUCCESS;
    Buffer = NULL;
    BufferSize = sizeof(EFI_MEMORY_DESCRIPTOR);

    //
    // Call the real function
    //

    while (GrowBuffer (&Status, (VOID **) &Buffer, BufferSize)) {
        Status = gBS->GetMemoryMap(&BufferSize, Buffer, MapKey, DescriptorSize, DescriptorVersion);
    }

    //
    // Convert buffer size to NoEntries
    //

    if (!EFI_ERROR(Status)) {
        *NoEntries = BufferSize / *DescriptorSize;
    }

    return Buffer;
}

//----------------------------------------------------------------------
// FindAcpiTable - searches the EFI info for the Acpi table.
//----------------------------------------------------------------------
#if 0
EFI_GUID gEfiAcpi20TableGuid = EFI_ACPI_20_TABLE_GUID;
EFI_GUID gEfiAcpi10TableGuid = EFI_ACPI_TABLE_GUID;
#endif

EFI_STATUS FindAcpiTable (IN EFI_SYSTEM_TABLE *pSystemTable, UINTN *pdwRSDPPtr)
{
    EFI_STATUS Status = EFI_SUCCESS;
    void *pRsdp = NULL;
    UINTN Index = 0;
    
    // found ACPI table RSD_PTR from system table
    for(Index = 0; Index < pSystemTable->NumberOfTableEntries; Index++) 
	{
		Print (L"Index %d  ", Index);
		PrintGUID (pSystemTable->ConfigurationTable[Index].VendorGuid);
		Print (L"\r\n");

		if(CompareGuid (&(pSystemTable->ConfigurationTable[Index].VendorGuid), &gEfiAcpi20TableGuid) ||
            CompareGuid (&(pSystemTable->ConfigurationTable[Index].VendorGuid), &gEfiAcpi10TableGuid)) 
		{
			//
			// A match was found.
			//
			pRsdp = g_pSystemTable->ConfigurationTable[Index].VendorTable;
			*pdwRSDPPtr = (UINTN)pRsdp;
			Print(L"FindAcpiTable: find Rsdp 0x%X\n", pRsdp);
			break;
		}
    }
    
    if(pRsdp == NULL) 
	{
        Print (L"FindAcpiTable: failed to find Rsdp!!!!\n");
        Status = EFI_NOT_FOUND;
    }
     
    return Status;
}

//----------------------------------------------------------------------
// Display the current Memory Map
//----------------------------------------------------------------------
void DumpMemoryMap (IN EFI_HANDLE ImageHandle)
{
	static CHAR16  *OsLoaderMemoryTypeDesc[EfiMaxMemoryType] = 
	{
        L"reserved  ",
        L"LoaderCode",
        L"LoaderData",
        L"BS_code   ",
        L"BS_data   ",
        L"RT_code   ",
        L"RT_data   ",
        L"available ",
        L"Unusable  ",
        L"ACPI_recl ",
        L"ACPI_NVS  ",
        L"MemMapIO  ",
		L"MemPortIO ",
		L"PAL_code  "
    };
    EFI_MEMORY_DESCRIPTOR   *MemoryMapEntry;
    EFI_MEMORY_DESCRIPTOR   *MemoryMap;
    UINTN                   NoEntries;
    UINTN                   MapKey;
    UINTN                   DescriptorSize;
    UINT32                  DescriptorVersion;
    UINT32                  i;


    // Get the memory map
    MemoryMap = LibMemoryMap(&NoEntries,&MapKey,&DescriptorSize,&DescriptorVersion);
    if (MemoryMap == NULL) {
        Print(L"Can not retrieve the current memory map\n");
        gBS->Exit(ImageHandle,EFI_SUCCESS,0,NULL);
    }

    Print(L"Memory Descriptor List:\n\n");
    Print(L"  Type        Start Address     End Address       Attributes      \n");
    Print(L"  ==========  ================  ================  ================\n");
    MemoryMapEntry = MemoryMap;
    for(i=0;i<NoEntries;i++) {
        Print(L"  %s  %lX  %lX  %lX\n",
              OsLoaderMemoryTypeDesc[MemoryMapEntry->Type],
              MemoryMapEntry->PhysicalStart,
              MemoryMapEntry->PhysicalStart+LShiftU64(MemoryMapEntry->NumberOfPages,EFI_PAGE_SHIFT)-1,
              MemoryMapEntry->Attribute);
        MemoryMapEntry = NEXT_MEMORY_DESCRIPTOR(MemoryMapEntry, DescriptorSize);
    }

    //
    // Free the resources allocated from pool.
    //
    FreePool(MemoryMap);
	return;
}

//----------------------------------------------------------------------
// DumpVidModes - Display the supported video modes
//----------------------------------------------------------------------
void DumpVidModes ()
{
	INT32 uMode, uMaxMode;
	UINTN uSizeOfInfo;
  //UINT16   key;
  UINT32 bpp = 8;
	UINT64 Tst;
	UINT8 *pTst;

    EFI_GRAPHICS_OUTPUT_PROTOCOL *pGraphicsOutput = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION    *Info;
    EFI_STATUS Status;

     // Find if there is VBE support (VESA)
    Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,
	                             NULL, (VOID**)&pGraphicsOutput);
    if (EFI_ERROR(Status)) 
	{
        Print (L"Failed to acquire GraphicsOutputProtoco handle,(Status = 0x%x)\n", Status);
        return ;
    }

	uMaxMode = pGraphicsOutput->Mode->MaxMode;
    for (uMode  = 0; uMode < uMaxMode; uMode++) 
	{
        Status = pGraphicsOutput->QueryMode(pGraphicsOutput, uMode,
                                            &uSizeOfInfo, &Info);
        if (EFI_ERROR(Status)) 
		{
            Print(L"Cannot get mode info (status=0x%x)\n", Status);
            break;
        }
        // Only support four bpp modes (8, 16, 24, 32) currently, and 20 resolutions for each
        if ((Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) ||
            (Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor))
              bpp = 24;
		{
            Print(L" [%d] %2d  (%d X %d)\r\n", uMode, bpp, Info->HorizontalResolution, Info->VerticalResolution);
        }
	}

	// Set mode 0 (on Minnowboard, that's 1920x1080
    Status = pGraphicsOutput->SetMode(pGraphicsOutput, 0);
    if (EFI_ERROR(Status)) 
	{
        Print(L"Cannot set mode (status=0x%x)\n", Status);
    }

	// Dump video information
	Print (L"Video mode        %d \r\n", (UINT32)pGraphicsOutput->Mode->Mode);
	Print (L"Video max mode    %d \r\n", (UINT32)pGraphicsOutput->Mode->MaxMode);
	Print (L"Video Width       %d \r\n", (UINT32)pGraphicsOutput->Mode->Info->HorizontalResolution);
	Print (L"Video Height      %d \r\n", (UINT32)pGraphicsOutput->Mode->Info->VerticalResolution);
	Print (L"Video pixFmt      %d \r\n", (UINT32)pGraphicsOutput->Mode->Info->PixelFormat);
	Print (L"Video Pixel       %x %x %x %x \r\n", pGraphicsOutput->Mode->Info->PixelInformation.RedMask,
		   pGraphicsOutput->Mode->Info->PixelInformation.GreenMask,
		   pGraphicsOutput->Mode->Info->PixelInformation.BlueMask,
		   pGraphicsOutput->Mode->Info->PixelInformation.ReservedMask);
	Print (L"Video Width       %d \r\n", (UINT32)pGraphicsOutput->Mode->Info->PixelsPerScanLine);

	Tst = pGraphicsOutput->Mode->FrameBufferBase;
	pTst = (UINT8 *)&Tst;
	Print (L"Framebuffer Low  %x \r\n", (UINT32)pGraphicsOutput->Mode->FrameBufferBase);

	Print (L"Framebuffer Low  %x \r\n", *(UINTN *)pTst);
	Print (L"Framebuffer High %x \r\n", *(UINTN *)(pTst+4));


	SetBootArgs(pGraphicsOutput->Mode->Info->HorizontalResolution, pGraphicsOutput->Mode->Info->VerticalResolution,
				 24, pGraphicsOutput->Mode->Info->PixelsPerScanLine * 4, 
				 (UINT32)pGraphicsOutput->Mode->FrameBufferBase);
	return;
}

#define IMAGE_SHARE_BOOT_ARGS_PA            0x001FF000
#define IMAGE_SHARE_BOOT_ARGS_PTR_PA        0x001FFFFC

//------------------------------------------------------------------------------
// SetBootArgs - Sets minimal boot args for NK.
//
void SetBootArgs(UINT32 dwDefVideoWidth, UINT32 dwDefVideoHeight, 
				 UINT32 dwDefVideoDepth, UINT32 dwStride, UINT32 pVideoBuff)
{
    BOOT_ARGS **ppBootArgs = (BOOT_ARGS **)IMAGE_SHARE_BOOT_ARGS_PTR_PA;
    pBootArgs = (BOOT_ARGS *)IMAGE_SHARE_BOOT_ARGS_PA;
	
    *ppBootArgs = pBootArgs;
    memset(pBootArgs, 0, sizeof(*pBootArgs));
    pBootArgs->dwSig = BOOTARG_SIG;
    pBootArgs->dwLen = sizeof(BOOT_ARGS);
    pBootArgs->dwVersionSig = BOOT_ARG_VERSION_SIG;
    pBootArgs->MajorVersion = BOOT_ARG_MAJOR_VER;
    pBootArgs->MinorVersion = BOOT_ARG_MINOR_VER;

	//
	// Video settings. The defaults can be overridden by fixup vars.
	//
	pBootArgs->cxDisplayScreen     = (WORD)dwDefVideoWidth;
	pBootArgs->cyDisplayScreen     = (WORD)dwDefVideoHeight;
	pBootArgs->bppScreen           = (WORD)dwDefVideoDepth;
	pBootArgs->cbScanLineLength    = dwStride;
	pBootArgs->pvFlatFrameBuffer   = pVideoBuff;

	return;
}
