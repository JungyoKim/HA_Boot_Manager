/** @file
  HA Boot Manager v9.0 - Auto OS Discovery + Setup TUI

  1. Load config from NVRAM (or defaults)
  2. 's' key → Setup screen (NVRAM-persistent config)
  3. Scan ESP for bootable OS entries
  4. Load network drivers from ESP
  5. Initialize network (Static IP / DHCP)
  6. Send discovered OS list and receive selection via TCP
  7. Boot selected OS or default OS on failure
**/

#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/Ip4Config2.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/Tcp4.h>
#include <Uefi.h>

#define DRIVER_PATH L"\\EFI\\HaBootManager\\drivers\\"
#define MAX_OS_ENTRIES 16
#define MAX_OS_NAME_LEN 32
#define MAX_OS_PATH_LEN 128

// ============================================================================
// Config
// ============================================================================

#define CONFIG_SIGNATURE 0x48414243  // "HABC"
#define CONFIG_VAR_NAME  L"HaBootConfig"

#define NETWORK_MODE_STATIC 0
#define NETWORK_MODE_DHCP   1

typedef struct {
  UINT32  Signature;
  UINT8   ServerIp[4];
  UINT16  ServerPort;
  UINT8   NetworkMode;      // 0=Static, 1=DHCP
  UINT8   StaticIp[4];
  UINT8   SubnetMask[4];
  UINT8   Gateway[4];
  UINT16  BootTimeout;      // seconds for setup key countdown
  CHAR16  DefaultOs[MAX_OS_NAME_LEN];
  UINT32  Checksum;
} HABOOT_CONFIG;

static EFI_GUID gHaBootConfigGuid = {
  0x48414243, 0x0001, 0x0001,
  {0x48, 0x41, 0x42, 0x4F, 0x4F, 0x54, 0x43, 0x46}
};

// ============================================================================
// OS Entry
// ============================================================================

typedef struct {
  CHAR16 Name[MAX_OS_NAME_LEN];
  CHAR16 Path[MAX_OS_PATH_LEN];
  BOOLEAN IsValid;
} OS_ENTRY;

OS_ENTRY gOsList[MAX_OS_ENTRIES];
UINTN gOsCount = 0;

typedef struct {
  CHAR16 *PathPattern;
  CHAR16 *OsName;
} OS_PATTERN;

OS_PATTERN gKnownPatterns[] = {
    {L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", L"Windows"},
    {L"\\EFI\\ubuntu\\shimx64.efi", L"Ubuntu"},
    {L"\\EFI\\fedora\\shimx64.efi", L"Fedora"},
    {L"\\EFI\\centos\\shimx64.efi", L"CentOS"},
    {L"\\EFI\\debian\\grubx64.efi", L"Debian"},
    {L"\\EFI\\arch\\grubx64.efi", L"Arch Linux"},
    {L"\\EFI\\manjaro\\grubx64.efi", L"Manjaro"},
    {L"\\EFI\\opensuse\\grubx64.efi", L"openSUSE"},
    {L"\\EFI\\pop\\shimx64.efi", L"Pop!_OS"},
    {L"\\EFI\\linuxmint\\grubx64.efi", L"Linux Mint"},
    {L"\\EFI\\systemd\\systemd-bootx64.efi", L"systemd-boot"},
    {NULL, NULL}};

CHAR16 *NetworkDrivers[] = {L"DpcDxe.efi",   L"SnpDxe.efi", L"MnpDxe.efi",
                            L"ArpDxe.efi",   L"Ip4Dxe.efi", L"Udp4Dxe.efi",
                            L"Dhcp4Dxe.efi", L"TcpDxe.efi", NULL};

BOOLEAN gTcpComplete = FALSE;

VOID EFIAPI TcpCallback(IN EFI_EVENT Event, IN VOID *Context) {
  gTcpComplete = TRUE;
}

// ============================================================================
// Config Functions
// ============================================================================

UINT32
ComputeConfigChecksum(IN HABOOT_CONFIG *Config) {
  UINT32 Sum = 0;
  UINT8 *Ptr = (UINT8 *)Config;
  UINTN Size = sizeof(HABOOT_CONFIG) - sizeof(UINT32);  // exclude Checksum field
  UINTN i;

  for (i = 0; i < Size; i++) {
    Sum += Ptr[i];
  }
  return Sum;
}

VOID
SetDefaultConfig(OUT HABOOT_CONFIG *Config) {
  ZeroMem(Config, sizeof(HABOOT_CONFIG));
  Config->Signature = CONFIG_SIGNATURE;

  Config->ServerIp[0] = 192;
  Config->ServerIp[1] = 168;
  Config->ServerIp[2] = 0;
  Config->ServerIp[3] = 67;
  Config->ServerPort = 9999;

  Config->NetworkMode = NETWORK_MODE_STATIC;

  Config->StaticIp[0] = 192;
  Config->StaticIp[1] = 168;
  Config->StaticIp[2] = 0;
  Config->StaticIp[3] = 114;

  Config->SubnetMask[0] = 255;
  Config->SubnetMask[1] = 255;
  Config->SubnetMask[2] = 255;
  Config->SubnetMask[3] = 0;

  Config->Gateway[0] = 192;
  Config->Gateway[1] = 168;
  Config->Gateway[2] = 0;
  Config->Gateway[3] = 1;

  Config->BootTimeout = 3;
  StrCpyS(Config->DefaultOs, MAX_OS_NAME_LEN, L"Windows");

  Config->Checksum = ComputeConfigChecksum(Config);
}

EFI_STATUS
LoadConfig(OUT HABOOT_CONFIG *Config) {
  EFI_STATUS Status;
  UINTN DataSize = sizeof(HABOOT_CONFIG);

  Status = gRT->GetVariable(
      CONFIG_VAR_NAME,
      &gHaBootConfigGuid,
      NULL,
      &DataSize,
      Config);

  if (EFI_ERROR(Status) || DataSize != sizeof(HABOOT_CONFIG)) {
    SetDefaultConfig(Config);
    return EFI_NOT_FOUND;
  }

  if (Config->Signature != CONFIG_SIGNATURE ||
      Config->Checksum != ComputeConfigChecksum(Config)) {
    SetDefaultConfig(Config);
    return EFI_CRC_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
SaveConfig(IN HABOOT_CONFIG *Config) {
  Config->Signature = CONFIG_SIGNATURE;
  Config->Checksum = ComputeConfigChecksum(Config);

  return gRT->SetVariable(
      CONFIG_VAR_NAME,
      &gHaBootConfigGuid,
      EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
      sizeof(HABOOT_CONFIG),
      Config);
}

// ============================================================================
// String Helpers
// ============================================================================

CHAR16
ToUpperCase(IN CHAR16 Char) {
  if (Char >= L'a' && Char <= L'z') {
    return Char - L'a' + L'A';
  }
  return Char;
}

INTN StrCmpNoCase(IN CHAR16 *Str1, IN CHAR16 *Str2) {
  while (*Str1 != L'\0' && *Str2 != L'\0') {
    if (ToUpperCase(*Str1) != ToUpperCase(*Str2)) {
      return ToUpperCase(*Str1) - ToUpperCase(*Str2);
    }
    Str1++;
    Str2++;
  }
  return ToUpperCase(*Str1) - ToUpperCase(*Str2);
}

BOOLEAN
FileExists(IN EFI_FILE_PROTOCOL *Root, IN CHAR16 *FilePath) {
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File;

  Status = Root->Open(Root, &File, FilePath, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR(Status)) {
    File->Close(File);
    return TRUE;
  }
  return FALSE;
}

CHAR16 *GetOsNameFromPath(IN CHAR16 *Path) {
  UINTN Index;

  for (Index = 0; gKnownPatterns[Index].PathPattern != NULL; Index++) {
    if (StrCmp(Path, gKnownPatterns[Index].PathPattern) == 0) {
      return gKnownPatterns[Index].OsName;
    }
  }
  return L"Unknown OS";
}

VOID AddOsEntry(IN CHAR16 *Name, IN CHAR16 *Path) {
  UINTN Index;

  if (gOsCount >= MAX_OS_ENTRIES)
    return;

  // Skip duplicates (same path)
  for (Index = 0; Index < gOsCount; Index++) {
    if (gOsList[Index].IsValid && StrCmp(gOsList[Index].Path, Path) == 0) {
      return;
    }
  }

  StrnCpyS(gOsList[gOsCount].Name, MAX_OS_NAME_LEN, Name, MAX_OS_NAME_LEN - 1);
  StrnCpyS(gOsList[gOsCount].Path, MAX_OS_PATH_LEN, Path, MAX_OS_PATH_LEN - 1);
  gOsList[gOsCount].IsValid = TRUE;
  gOsCount++;
}

// ============================================================================
// OS Scanning
// ============================================================================

EFI_STATUS
ScanBootableOS(VOID) {
  EFI_STATUS Status;
  EFI_HANDLE *HandleBuffer;
  UINTN HandleCount;
  UINTN HandleIndex;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_FILE_PROTOCOL *Root;
  UINTN PatternIndex;

  gOsCount = 0;
  ZeroMem(gOsList, sizeof(gOsList));

  Status =
      gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                              NULL, &HandleCount, &HandleBuffer);
  if (EFI_ERROR(Status))
    return Status;

  for (HandleIndex = 0; HandleIndex < HandleCount; HandleIndex++) {
    Status = gBS->HandleProtocol(HandleBuffer[HandleIndex],
                                 &gEfiSimpleFileSystemProtocolGuid,
                                 (VOID **)&FileSystem);
    if (EFI_ERROR(Status))
      continue;

    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status))
      continue;

    for (PatternIndex = 0; gKnownPatterns[PatternIndex].PathPattern != NULL;
         PatternIndex++) {
      if (FileExists(Root, gKnownPatterns[PatternIndex].PathPattern)) {
        if (StrStr(gKnownPatterns[PatternIndex].PathPattern,
                   L"HaBootManager") != NULL) {
          continue;
        }
        AddOsEntry(gKnownPatterns[PatternIndex].OsName,
                   gKnownPatterns[PatternIndex].PathPattern);
      }
    }

    Root->Close(Root);
  }

  FreePool(HandleBuffer);
  AddOsEntry(L"Menu", L"MENU");

  return EFI_SUCCESS;
}

OS_ENTRY *FindOsByName(IN CHAR8 *Name) {
  UINTN Index;
  CHAR16 WideName[MAX_OS_NAME_LEN];

  for (Index = 0; Index < MAX_OS_NAME_LEN - 1 && Name[Index] != '\0'; Index++) {
    WideName[Index] = (CHAR16)Name[Index];
  }
  WideName[Index] = L'\0';

  for (Index = 0; Index < gOsCount; Index++) {
    if (gOsList[Index].IsValid) {
      if (StrCmpNoCase(gOsList[Index].Name, WideName) == 0) {
        return &gOsList[Index];
      }
    }
  }

  for (Index = 0; Index < gOsCount; Index++) {
    if (gOsList[Index].IsValid) {
      if (StrStr(gOsList[Index].Name, WideName) != NULL) {
        return &gOsList[Index];
      }
    }
  }

  return NULL;
}

OS_ENTRY *FindOsByNameW(IN CHAR16 *Name) {
  UINTN Index;

  for (Index = 0; Index < gOsCount; Index++) {
    if (gOsList[Index].IsValid) {
      if (StrCmpNoCase(gOsList[Index].Name, Name) == 0) {
        return &gOsList[Index];
      }
    }
  }
  for (Index = 0; Index < gOsCount; Index++) {
    if (gOsList[Index].IsValid) {
      if (StrStr(gOsList[Index].Name, Name) != NULL) {
        return &gOsList[Index];
      }
    }
  }
  return NULL;
}

// ============================================================================
// Driver Loading
// ============================================================================

EFI_STATUS
LoadDriver(IN EFI_HANDLE ImageHandle, IN CHAR16 *DriverPath) {
  EFI_STATUS Status;
  EFI_HANDLE *HandleBuffer;
  UINTN HandleCount;
  UINTN Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_DEVICE_PATH_PROTOCOL *DevPath;
  EFI_HANDLE DriverHandle;

  Status =
      gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                              NULL, &HandleCount, &HandleBuffer);
  if (EFI_ERROR(Status))
    return Status;

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol(HandleBuffer[Index],
                                 &gEfiSimpleFileSystemProtocolGuid,
                                 (VOID **)&FileSystem);
    if (EFI_ERROR(Status))
      continue;

    EFI_FILE_PROTOCOL *Root, *File;
    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status))
      continue;

    Status = Root->Open(Root, &File, DriverPath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
      Root->Close(Root);
      continue;
    }
    File->Close(File);
    Root->Close(Root);

    DevPath = FileDevicePath(HandleBuffer[Index], DriverPath);
    if (DevPath == NULL)
      continue;

    Status =
        gBS->LoadImage(FALSE, ImageHandle, DevPath, NULL, 0, &DriverHandle);
    FreePool(DevPath);
    if (EFI_ERROR(Status))
      continue;

    gBS->StartImage(DriverHandle, NULL, NULL);
  }

  FreePool(HandleBuffer);
  return EFI_SUCCESS;
}

EFI_STATUS
LoadNetworkDrivers(IN EFI_HANDLE ImageHandle) {
  UINTN Index;
  CHAR16 FullPath[256];

  for (Index = 0; NetworkDrivers[Index] != NULL; Index++) {
    FullPath[0] = 0;
    StrCatS(FullPath, sizeof(FullPath) / sizeof(CHAR16), DRIVER_PATH);
    StrCatS(FullPath, sizeof(FullPath) / sizeof(CHAR16), NetworkDrivers[Index]);
    LoadDriver(ImageHandle, FullPath);
  }
  return EFI_SUCCESS;
}

VOID ConnectAllControllers(VOID) {
  EFI_STATUS Status;
  UINTN HandleCount;
  EFI_HANDLE *HandleBuffer;
  UINTN Index;

  Status = gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &HandleCount,
                                   &HandleBuffer);
  if (EFI_ERROR(Status))
    return;

  for (Index = 0; Index < HandleCount; Index++) {
    gBS->ConnectController(HandleBuffer[Index], NULL, NULL, TRUE);
  }
  FreePool(HandleBuffer);
}

// ============================================================================
// Network (Config-aware)
// ============================================================================

EFI_STATUS
WaitForNetworkWithConfig(IN HABOOT_CONFIG *Config) {
  EFI_STATUS Status;
  EFI_HANDLE *HandleBuffer;
  UINTN HandleCount;
  EFI_IP4_CONFIG2_PROTOCOL *Ip4Config2;
  UINTN DataSize;
  EFI_IP4_CONFIG2_INTERFACE_INFO *IfInfo;
  UINTN Retry;
  EFI_IP4_CONFIG2_POLICY Policy;
  EFI_IP4_CONFIG2_MANUAL_ADDRESS ManualAddress;

  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiIp4Config2ProtocolGuid,
                                   NULL, &HandleCount, &HandleBuffer);
  if (EFI_ERROR(Status) || HandleCount == 0)
    return EFI_NOT_FOUND;

  Status = gBS->HandleProtocol(HandleBuffer[0], &gEfiIp4Config2ProtocolGuid,
                               (VOID **)&Ip4Config2);
  FreePool(HandleBuffer);
  if (EFI_ERROR(Status))
    return Status;

  if (Config->NetworkMode == NETWORK_MODE_STATIC) {
    Policy = Ip4Config2PolicyStatic;
    Status = Ip4Config2->SetData(Ip4Config2, Ip4Config2DataTypePolicy,
                                 sizeof(Policy), &Policy);
    if (!EFI_ERROR(Status)) {
      ZeroMem(&ManualAddress, sizeof(ManualAddress));
      ManualAddress.Address.Addr[0] = Config->StaticIp[0];
      ManualAddress.Address.Addr[1] = Config->StaticIp[1];
      ManualAddress.Address.Addr[2] = Config->StaticIp[2];
      ManualAddress.Address.Addr[3] = Config->StaticIp[3];
      ManualAddress.SubnetMask.Addr[0] = Config->SubnetMask[0];
      ManualAddress.SubnetMask.Addr[1] = Config->SubnetMask[1];
      ManualAddress.SubnetMask.Addr[2] = Config->SubnetMask[2];
      ManualAddress.SubnetMask.Addr[3] = Config->SubnetMask[3];

      Status = Ip4Config2->SetData(Ip4Config2, Ip4Config2DataTypeManualAddress,
                                   sizeof(ManualAddress), &ManualAddress);
      if (!EFI_ERROR(Status)) {
        EFI_IPv4_ADDRESS GatewayAddr;
        GatewayAddr.Addr[0] = Config->Gateway[0];
        GatewayAddr.Addr[1] = Config->Gateway[1];
        GatewayAddr.Addr[2] = Config->Gateway[2];
        GatewayAddr.Addr[3] = Config->Gateway[3];
        Ip4Config2->SetData(Ip4Config2, Ip4Config2DataTypeGateway,
                            sizeof(GatewayAddr), &GatewayAddr);
        gBS->Stall(500000);
        return EFI_SUCCESS;
      }
    }
  }

  // DHCP mode or static fallback
  Policy = Ip4Config2PolicyDhcp;
  Status = Ip4Config2->SetData(Ip4Config2, Ip4Config2DataTypePolicy,
                               sizeof(Policy), &Policy);
  if (EFI_ERROR(Status))
    return Status;

  for (Retry = 0; Retry < 30; Retry++) {
    DataSize = 0;
    Status = Ip4Config2->GetData(Ip4Config2, Ip4Config2DataTypeInterfaceInfo,
                                 &DataSize, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL && DataSize > 0) {
      IfInfo = AllocatePool(DataSize);
      Status = Ip4Config2->GetData(Ip4Config2, Ip4Config2DataTypeInterfaceInfo,
                                   &DataSize, IfInfo);
      if (!EFI_ERROR(Status) && IfInfo->StationAddress.Addr[0] != 0) {
        FreePool(IfInfo);
        return EFI_SUCCESS;
      }
      FreePool(IfInfo);
    }
    gBS->Stall(1000000);
  }
  return EFI_TIMEOUT;
}

// ============================================================================
// TCP (Config-aware)
// ============================================================================

VOID BuildOsListString(OUT CHAR8 *Buffer, IN UINTN BufferSize) {
  UINTN Index;
  UINTN Offset = 0;

  Buffer[0] = '\0';

  for (Index = 0; Index < gOsCount && Offset < BufferSize - 50; Index++) {
    if (gOsList[Index].IsValid) {
      if (Index > 0) {
        Buffer[Offset++] = ',';
      }
      UINTN NameLen = StrLen(gOsList[Index].Name);
      for (UINTN j = 0; j < NameLen && Offset < BufferSize - 1; j++) {
        Buffer[Offset++] = (CHAR8)gOsList[Index].Name[j];
      }
    }
  }
  Buffer[Offset] = '\0';
}

EFI_STATUS
QueryTcpServerWithConfig(IN HABOOT_CONFIG *Config, OUT OS_ENTRY **SelectedOs) {
  EFI_STATUS Status;
  EFI_SERVICE_BINDING_PROTOCOL *TcpSb;
  EFI_TCP4_PROTOCOL *Tcp4;
  EFI_HANDLE TcpHandle;
  EFI_HANDLE *HandleBuffer;
  UINTN HandleCount;
  EFI_TCP4_CONFIG_DATA ConfigData;
  EFI_TCP4_ACCESS_POINT AccessPoint;
  EFI_TCP4_OPTION ControlOption;
  EFI_TCP4_CONNECTION_TOKEN ConnectToken;
  EFI_TCP4_IO_TOKEN SendToken, RecvToken;
  EFI_TCP4_TRANSMIT_DATA SendData;
  EFI_TCP4_RECEIVE_DATA RecvData;
  CHAR8 SendBuffer[512];
  CHAR8 RecvBuffer[64];
  UINTN Retry;

  *SelectedOs = NULL;

  Status =
      gBS->LocateHandleBuffer(ByProtocol, &gEfiTcp4ServiceBindingProtocolGuid,
                              NULL, &HandleCount, &HandleBuffer);
  if (EFI_ERROR(Status) || HandleCount == 0)
    return Status;

  Status = gBS->HandleProtocol(
      HandleBuffer[0], &gEfiTcp4ServiceBindingProtocolGuid, (VOID **)&TcpSb);
  FreePool(HandleBuffer);
  if (EFI_ERROR(Status))
    return Status;

  TcpHandle = NULL;
  Status = TcpSb->CreateChild(TcpSb, &TcpHandle);
  if (EFI_ERROR(Status))
    return Status;

  Status =
      gBS->HandleProtocol(TcpHandle, &gEfiTcp4ProtocolGuid, (VOID **)&Tcp4);
  if (EFI_ERROR(Status)) {
    TcpSb->DestroyChild(TcpSb, TcpHandle);
    return Status;
  }

  ZeroMem(&AccessPoint, sizeof(AccessPoint));
  AccessPoint.UseDefaultAddress = TRUE;
  AccessPoint.StationPort = 0;
  AccessPoint.RemoteAddress.Addr[0] = Config->ServerIp[0];
  AccessPoint.RemoteAddress.Addr[1] = Config->ServerIp[1];
  AccessPoint.RemoteAddress.Addr[2] = Config->ServerIp[2];
  AccessPoint.RemoteAddress.Addr[3] = Config->ServerIp[3];
  AccessPoint.RemotePort = Config->ServerPort;
  AccessPoint.ActiveFlag = TRUE;

  ZeroMem(&ControlOption, sizeof(ControlOption));
  ControlOption.ReceiveBufferSize = 4096;
  ControlOption.SendBufferSize = 4096;
  ControlOption.ConnectionTimeout = 10;
  ControlOption.DataRetries = 3;
  ControlOption.FinTimeout = 10;
  ControlOption.TimeWaitTimeout = 10;

  ZeroMem(&ConfigData, sizeof(ConfigData));
  ConfigData.TypeOfService = 0;
  ConfigData.TimeToLive = 64;
  ConfigData.AccessPoint = AccessPoint;
  ConfigData.ControlOption = &ControlOption;

  Status = Tcp4->Configure(Tcp4, &ConfigData);
  if (EFI_ERROR(Status)) {
    TcpSb->DestroyChild(TcpSb, TcpHandle);
    return Status;
  }

  // Connect
  ZeroMem(&ConnectToken, sizeof(ConnectToken));
  gTcpComplete = FALSE;
  gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TcpCallback, NULL,
                   &ConnectToken.CompletionToken.Event);

  Status = Tcp4->Connect(Tcp4, &ConnectToken);
  if (EFI_ERROR(Status)) {
    gBS->CloseEvent(ConnectToken.CompletionToken.Event);
    Tcp4->Configure(Tcp4, NULL);
    TcpSb->DestroyChild(TcpSb, TcpHandle);
    return Status;
  }

  for (Retry = 0; Retry < 100 && !gTcpComplete; Retry++) {
    gBS->Stall(100000);
  }

  if (!gTcpComplete || EFI_ERROR(ConnectToken.CompletionToken.Status)) {
    gBS->CloseEvent(ConnectToken.CompletionToken.Event);
    Tcp4->Configure(Tcp4, NULL);
    TcpSb->DestroyChild(TcpSb, TcpHandle);
    return EFI_TIMEOUT;
  }
  gBS->CloseEvent(ConnectToken.CompletionToken.Event);

  // Send OS list
  BuildOsListString(SendBuffer, sizeof(SendBuffer));

  ZeroMem(&SendData, sizeof(SendData));
  SendData.Push = TRUE;
  SendData.Urgent = FALSE;
  SendData.DataLength = (UINT32)AsciiStrLen(SendBuffer);
  SendData.FragmentCount = 1;
  SendData.FragmentTable[0].FragmentLength = SendData.DataLength;
  SendData.FragmentTable[0].FragmentBuffer = SendBuffer;

  ZeroMem(&SendToken, sizeof(SendToken));
  SendToken.Packet.TxData = &SendData;
  gTcpComplete = FALSE;
  gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TcpCallback, NULL,
                   &SendToken.CompletionToken.Event);

  Status = Tcp4->Transmit(Tcp4, &SendToken);
  if (!EFI_ERROR(Status)) {
    for (Retry = 0; Retry < 50 && !gTcpComplete; Retry++) {
      gBS->Stall(100000);
    }
  }
  gBS->CloseEvent(SendToken.CompletionToken.Event);

  // Receive selection
  ZeroMem(RecvBuffer, sizeof(RecvBuffer));
  ZeroMem(&RecvData, sizeof(RecvData));
  RecvData.UrgentFlag = FALSE;
  RecvData.DataLength = sizeof(RecvBuffer) - 1;
  RecvData.FragmentCount = 1;
  RecvData.FragmentTable[0].FragmentLength = sizeof(RecvBuffer) - 1;
  RecvData.FragmentTable[0].FragmentBuffer = RecvBuffer;

  ZeroMem(&RecvToken, sizeof(RecvToken));
  RecvToken.Packet.RxData = &RecvData;
  gTcpComplete = FALSE;
  gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TcpCallback, NULL,
                   &RecvToken.CompletionToken.Event);

  Status = Tcp4->Receive(Tcp4, &RecvToken);
  if (EFI_ERROR(Status)) {
    gBS->CloseEvent(RecvToken.CompletionToken.Event);
    Tcp4->Configure(Tcp4, NULL);
    TcpSb->DestroyChild(TcpSb, TcpHandle);
    return Status;
  }

  for (Retry = 0; Retry < 50 && !gTcpComplete; Retry++) {
    gBS->Stall(100000);
  }

  if (gTcpComplete && !EFI_ERROR(RecvToken.CompletionToken.Status)) {
    *SelectedOs = FindOsByName(RecvBuffer);
  }

  gBS->CloseEvent(RecvToken.CompletionToken.Event);
  Tcp4->Configure(Tcp4, NULL);
  TcpSb->DestroyChild(TcpSb, TcpHandle);

  return EFI_SUCCESS;
}

// ============================================================================
// Test Connection (TCP connect-only test)
// ============================================================================

EFI_STATUS
TestConnection(IN HABOOT_CONFIG *Config) {
  EFI_STATUS Status;
  EFI_SERVICE_BINDING_PROTOCOL *TcpSb;
  EFI_TCP4_PROTOCOL *Tcp4;
  EFI_HANDLE TcpHandle;
  EFI_HANDLE *HandleBuffer;
  UINTN HandleCount;
  EFI_TCP4_CONFIG_DATA CfgData;
  EFI_TCP4_ACCESS_POINT Ap;
  EFI_TCP4_OPTION Opt;
  EFI_TCP4_CONNECTION_TOKEN ConnToken;
  UINTN Retry;

  Status =
      gBS->LocateHandleBuffer(ByProtocol, &gEfiTcp4ServiceBindingProtocolGuid,
                              NULL, &HandleCount, &HandleBuffer);
  if (EFI_ERROR(Status) || HandleCount == 0)
    return EFI_NOT_FOUND;

  Status = gBS->HandleProtocol(
      HandleBuffer[0], &gEfiTcp4ServiceBindingProtocolGuid, (VOID **)&TcpSb);
  FreePool(HandleBuffer);
  if (EFI_ERROR(Status))
    return Status;

  TcpHandle = NULL;
  Status = TcpSb->CreateChild(TcpSb, &TcpHandle);
  if (EFI_ERROR(Status))
    return Status;

  Status =
      gBS->HandleProtocol(TcpHandle, &gEfiTcp4ProtocolGuid, (VOID **)&Tcp4);
  if (EFI_ERROR(Status)) {
    TcpSb->DestroyChild(TcpSb, TcpHandle);
    return Status;
  }

  ZeroMem(&Ap, sizeof(Ap));
  Ap.UseDefaultAddress = TRUE;
  Ap.StationPort = 0;
  Ap.RemoteAddress.Addr[0] = Config->ServerIp[0];
  Ap.RemoteAddress.Addr[1] = Config->ServerIp[1];
  Ap.RemoteAddress.Addr[2] = Config->ServerIp[2];
  Ap.RemoteAddress.Addr[3] = Config->ServerIp[3];
  Ap.RemotePort = Config->ServerPort;
  Ap.ActiveFlag = TRUE;

  ZeroMem(&Opt, sizeof(Opt));
  Opt.ReceiveBufferSize = 4096;
  Opt.SendBufferSize = 4096;
  Opt.ConnectionTimeout = 5;
  Opt.DataRetries = 2;
  Opt.FinTimeout = 5;
  Opt.TimeWaitTimeout = 5;

  ZeroMem(&CfgData, sizeof(CfgData));
  CfgData.TypeOfService = 0;
  CfgData.TimeToLive = 64;
  CfgData.AccessPoint = Ap;
  CfgData.ControlOption = &Opt;

  // Try configure, retry if network not ready yet
  {
    UINTN CfgRetry;
    for (CfgRetry = 0; CfgRetry < 30; CfgRetry++) {
      Status = Tcp4->Configure(Tcp4, &CfgData);
      if (!EFI_ERROR(Status)) break;
      gBS->Stall(200000);  // 200ms wait for network init
    }
  }
  if (EFI_ERROR(Status)) {
    TcpSb->DestroyChild(TcpSb, TcpHandle);
    return Status;
  }

  ZeroMem(&ConnToken, sizeof(ConnToken));
  gTcpComplete = FALSE;
  gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TcpCallback, NULL,
                   &ConnToken.CompletionToken.Event);

  Status = Tcp4->Connect(Tcp4, &ConnToken);
  if (EFI_ERROR(Status)) {
    gBS->CloseEvent(ConnToken.CompletionToken.Event);
    Tcp4->Configure(Tcp4, NULL);
    TcpSb->DestroyChild(TcpSb, TcpHandle);
    return Status;
  }

  for (Retry = 0; Retry < 50 && !gTcpComplete; Retry++) {
    gBS->Stall(100000);
  }

  Status = EFI_TIMEOUT;
  if (gTcpComplete && !EFI_ERROR(ConnToken.CompletionToken.Status)) {
    Status = EFI_SUCCESS;
  }

  gBS->CloseEvent(ConnToken.CompletionToken.Event);
  Tcp4->Configure(Tcp4, NULL);
  TcpSb->DestroyChild(TcpSb, TcpHandle);
  return Status;
}

// ============================================================================
// Display Functions
// ============================================================================

VOID
SetMaxGopResolution(VOID) {
  EFI_STATUS Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR(Status)) return;

  UINT32 MaxMode = 0;
  UINTN MaxPixels = 0;
  UINT32 Mode;
  for (Mode = 0; Mode < Gop->Mode->MaxMode; Mode++) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN InfoSize;
    if (!EFI_ERROR(Gop->QueryMode(Gop, Mode, &InfoSize, &Info))) {
      UINTN Pixels = (UINTN)Info->HorizontalResolution * Info->VerticalResolution;
      if (Pixels > MaxPixels) {
        MaxPixels = Pixels;
        MaxMode = Mode;
      }
    }
  }
  if (MaxMode != Gop->Mode->Mode) {
    Gop->SetMode(Gop, MaxMode);
  }
}

// ============================================================================
// Boot Functions
// ============================================================================

EFI_STATUS
BootEfiFile(IN EFI_HANDLE ImageHandle, IN CHAR16 *FilePath) {
  EFI_STATUS Status;
  EFI_HANDLE *HandleBuffer;
  UINTN HandleCount;
  UINTN Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_DEVICE_PATH_PROTOCOL *FileDevPath;
  EFI_HANDLE NewImageHandle;

  Status =
      gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                              NULL, &HandleCount, &HandleBuffer);
  if (EFI_ERROR(Status))
    return Status;

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol(HandleBuffer[Index],
                                 &gEfiSimpleFileSystemProtocolGuid,
                                 (VOID **)&FileSystem);
    if (EFI_ERROR(Status))
      continue;

    EFI_FILE_PROTOCOL *Root, *File;
    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status))
      continue;

    Status = Root->Open(Root, &File, FilePath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
      Root->Close(Root);
      continue;
    }

    File->Close(File);
    Root->Close(Root);

    FileDevPath = FileDevicePath(HandleBuffer[Index], FilePath);
    if (FileDevPath == NULL)
      continue;

    Status = gBS->LoadImage(FALSE, ImageHandle, FileDevPath, NULL, 0,
                            &NewImageHandle);
    FreePool(FileDevPath);
    if (EFI_ERROR(Status))
      continue;

    // Restore max GOP resolution before booting next OS
    SetMaxGopResolution();

    gBS->StartImage(NewImageHandle, NULL, NULL);
    FreePool(HandleBuffer);
    return EFI_SUCCESS;
  }

  FreePool(HandleBuffer);
  return EFI_NOT_FOUND;
}

EFI_STATUS
BootDefaultOs(IN EFI_HANDLE ImageHandle, IN HABOOT_CONFIG *Config) {
  OS_ENTRY *Entry = FindOsByNameW(Config->DefaultOs);
  if (Entry == NULL || StrCmp(Entry->Path, L"MENU") == 0) {
    return EFI_NOT_FOUND;
  }
  return BootEfiFile(ImageHandle, Entry->Path);
}

// ============================================================================
// TUI Setup Screen
// ============================================================================

// Box-drawing characters (UCS-2)
#define BOX_TL      0x250C  // ┌
#define BOX_TR      0x2510  // ┐
#define BOX_BL      0x2514  // └
#define BOX_BR      0x2518  // ┘
#define BOX_H       0x2500  // ─
#define BOX_V       0x2502  // │
#define BOX_LT      0x251C  // ├
#define BOX_RT      0x2524  // ┤

// Double-line box-drawing for splash screen
#define BOX2_TL     0x2554  // ╔
#define BOX2_TR     0x2557  // ╗
#define BOX2_BL     0x255A  // ╚
#define BOX2_BR     0x255D  // ╝
#define BOX2_H      0x2550  // ═
#define BOX2_V      0x2551  // ║

// Modern dark theme colors
#define COLOR_NORMAL     EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define COLOR_TITLE      EFI_TEXT_ATTR(EFI_WHITE, EFI_BLACK)
#define COLOR_HIGHLIGHT  EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY)
#define COLOR_EDITING    EFI_TEXT_ATTR(EFI_CYAN, EFI_BLACK)
#define COLOR_OCTET_CUR  EFI_TEXT_ATTR(EFI_BLACK, EFI_CYAN)
#define COLOR_BORDER     EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define COLOR_LABEL      EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define COLOR_VALUE      EFI_TEXT_ATTR(EFI_WHITE, EFI_BLACK)
#define COLOR_SECTION    EFI_TEXT_ATTR(EFI_CYAN, EFI_BLACK)
#define COLOR_BUTTON     EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define COLOR_BTN_HL     EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY)
#define COLOR_STATUS_OK  EFI_TEXT_ATTR(EFI_GREEN, EFI_BLACK)
#define COLOR_STATUS_ERR EFI_TEXT_ATTR(EFI_LIGHTRED, EFI_BLACK)
#define COLOR_FOOTER     EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define COLOR_ARROW      EFI_TEXT_ATTR(EFI_CYAN, EFI_BLACK)
#define COLOR_SPLASH_BDR EFI_TEXT_ATTR(EFI_CYAN, EFI_BLACK)
#define COLOR_SPLASH_DIM EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define COLOR_SPLASH_TXT EFI_TEXT_ATTR(EFI_WHITE, EFI_BLACK)
#define COLOR_SPLASH_KEY EFI_TEXT_ATTR(EFI_CYAN, EFI_BLACK)

// Setup field IDs
#define FIELD_SERVER_IP      0
#define FIELD_SERVER_PORT    1
#define FIELD_NET_MODE       2
#define FIELD_STATIC_IP      3
#define FIELD_SUBNET_MASK    4
#define FIELD_GATEWAY        5
#define FIELD_BOOT_TIMEOUT   6
#define FIELD_DEFAULT_OS     7
#define FIELD_BTN_TEST       8
#define FIELD_BTN_SAVE       9
#define FIELD_BTN_EXIT       10
#define FIELD_COUNT          11

// Box dimensions
#define BOX_WIDTH   60
#define BOX_HEIGHT  21

VOID
TuiPrintAt(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN Col,
  IN UINTN Row,
  IN CHAR16 *Str
  )
{
  ConOut->SetCursorPosition(ConOut, Col, Row);
  ConOut->OutputString(ConOut, Str);
}

VOID
TuiDrawHLine(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN StartCol,
  IN UINTN Row,
  IN UINTN Width,
  IN CHAR16 Left,
  IN CHAR16 Fill,
  IN CHAR16 Right
  )
{
  CHAR16 Buf[128];
  UINTN i;

  if (Width > 126) Width = 126;
  Buf[0] = Left;
  for (i = 1; i < Width - 1; i++) {
    Buf[i] = Fill;
  }
  Buf[Width - 1] = Right;
  Buf[Width] = L'\0';

  ConOut->SetCursorPosition(ConOut, StartCol, Row);
  ConOut->OutputString(ConOut, Buf);
}

VOID
TuiDrawEmptyLine(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN StartCol,
  IN UINTN Row,
  IN UINTN Width
  )
{
  CHAR16 Buf[128];
  UINTN i;

  if (Width > 126) Width = 126;
  Buf[0] = (CHAR16)BOX_V;
  for (i = 1; i < Width - 1; i++) {
    Buf[i] = L' ';
  }
  Buf[Width - 1] = (CHAR16)BOX_V;
  Buf[Width] = L'\0';

  ConOut->SetCursorPosition(ConOut, StartCol, Row);
  ConOut->OutputString(ConOut, Buf);
}

VOID
TuiFormatIp(OUT CHAR16 *Buf, IN UINTN BufSize, IN UINT8 *Ip) {
  UnicodeSPrint(Buf, BufSize, L"%3d.%3d.%3d.%3d",
                Ip[0], Ip[1], Ip[2], Ip[3]);
}

// Draw IP with per-octet highlighting for editing
VOID
TuiDrawIpWithCursor(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN Col,
  IN UINTN Row,
  IN UINT8 *Ip,
  IN INTN CurrentOctet,  // -1 = no cursor
  IN UINTN NormalAttr,
  IN UINTN CursorAttr
  )
{
  CHAR16 Octet[5];
  UINTN i;

  ConOut->SetCursorPosition(ConOut, Col, Row);
  for (i = 0; i < 4; i++) {
    UnicodeSPrint(Octet, sizeof(Octet), L"%3d", Ip[i]);
    ConOut->SetAttribute(ConOut, ((INTN)i == CurrentOctet) ? CursorAttr : NormalAttr);
    ConOut->OutputString(ConOut, Octet);
    if (i < 3) {
      ConOut->SetAttribute(ConOut, NormalAttr);
      ConOut->OutputString(ConOut, L".");
    }
  }
}

VOID
TuiDrawFrame(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y
  )
{
  UINTN Row;

  ConOut->SetAttribute(ConOut, COLOR_BORDER);

  // Top border
  TuiDrawHLine(ConOut, X, Y, BOX_WIDTH, (CHAR16)BOX_TL, (CHAR16)BOX_H, (CHAR16)BOX_TR);

  // Title row
  TuiDrawEmptyLine(ConOut, X, Y + 1, BOX_WIDTH);
  {
    CHAR16 *Title = L"HA BOOT MANAGER SETUP";
    UINTN TitleCol = X + (BOX_WIDTH - StrLen(Title)) / 2;
    ConOut->SetAttribute(ConOut, COLOR_TITLE);
    TuiPrintAt(ConOut, TitleCol, Y + 1, Title);
  }

  // Title separator
  ConOut->SetAttribute(ConOut, COLOR_BORDER);
  TuiDrawHLine(ConOut, X, Y + 2, BOX_WIDTH, (CHAR16)BOX_LT, (CHAR16)BOX_H, (CHAR16)BOX_RT);

  // Content area (rows 3..BOX_HEIGHT-3)
  for (Row = Y + 3; Row < Y + BOX_HEIGHT - 2; Row++) {
    TuiDrawEmptyLine(ConOut, X, Row, BOX_WIDTH);
  }

  // Footer separator
  TuiDrawHLine(ConOut, X, Y + BOX_HEIGHT - 2, BOX_WIDTH, (CHAR16)BOX_LT, (CHAR16)BOX_H, (CHAR16)BOX_RT);

  // Footer row
  TuiDrawEmptyLine(ConOut, X, Y + BOX_HEIGHT - 1, BOX_WIDTH);

  // Bottom border
  TuiDrawHLine(ConOut, X, Y + BOX_HEIGHT, BOX_WIDTH, (CHAR16)BOX_BL, (CHAR16)BOX_H, (CHAR16)BOX_BR);
}

VOID
TuiDrawField(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y,
  IN UINTN FieldId,
  IN UINTN SelectedField,
  IN BOOLEAN IsEditing,
  IN HABOOT_CONFIG *Config
  )
{
  CHAR16 Label[40];
  CHAR16 Value[60];
  UINTN ArrowCol = X + 2;
  UINTN LabelCol = X + 4;
  UINTN ValueCol = X + 28;
  UINTN Row;
  UINTN Attr;
  BOOLEAN IsSelected = (FieldId == SelectedField);
  BOOLEAN IsIpField = FALSE;
  UINT8 *IpData = NULL;

  switch (FieldId) {
  case FIELD_SERVER_IP:
    Row = Y + 4;
    StrCpyS(Label, 40, L"Server IP");
    IsIpField = TRUE;
    IpData = Config->ServerIp;
    break;
  case FIELD_SERVER_PORT:
    Row = Y + 5;
    StrCpyS(Label, 40, L"Server Port");
    UnicodeSPrint(Value, sizeof(Value), L"%d", Config->ServerPort);
    break;
  case FIELD_NET_MODE:
    Row = Y + 7;
    StrCpyS(Label, 40, L"Network Mode");
    if (Config->NetworkMode == NETWORK_MODE_STATIC) {
      StrCpyS(Value, 60, L"< Static >");
    } else {
      StrCpyS(Value, 60, L"<  DHCP  >");
    }
    break;
  case FIELD_STATIC_IP:
    Row = Y + 8;
    StrCpyS(Label, 40, L"Static IP");
    IsIpField = TRUE;
    IpData = Config->StaticIp;
    break;
  case FIELD_SUBNET_MASK:
    Row = Y + 9;
    StrCpyS(Label, 40, L"Subnet Mask");
    IsIpField = TRUE;
    IpData = Config->SubnetMask;
    break;
  case FIELD_GATEWAY:
    Row = Y + 10;
    StrCpyS(Label, 40, L"Gateway");
    IsIpField = TRUE;
    IpData = Config->Gateway;
    break;
  case FIELD_BOOT_TIMEOUT:
    Row = Y + 12;
    StrCpyS(Label, 40, L"Boot Timeout");
    UnicodeSPrint(Value, sizeof(Value), L"%-3d sec  ", Config->BootTimeout);
    break;
  case FIELD_DEFAULT_OS:
    Row = Y + 13;
    StrCpyS(Label, 40, L"Default OS");
    UnicodeSPrint(Value, sizeof(Value), L"< %-20s >", Config->DefaultOs);
    break;
  case FIELD_BTN_TEST:
    Row = Y + 15;
    {
      CHAR16 *Btn = L"[ Test Connection ]";
      UINTN BtnCol = X + (BOX_WIDTH - StrLen(Btn)) / 2;
      ConOut->SetAttribute(ConOut, IsSelected ? COLOR_BTN_HL : COLOR_BUTTON);
      TuiPrintAt(ConOut, BtnCol, Row, Btn);
    }
    return;
  case FIELD_BTN_SAVE:
    Row = Y + 18;
    {
      CHAR16 *Btn = L"[ Save & Exit ]";
      UINTN BtnCol = X + BOX_WIDTH / 2 - StrLen(Btn) - 1;
      ConOut->SetAttribute(ConOut, IsSelected ? COLOR_BTN_HL : COLOR_BUTTON);
      TuiPrintAt(ConOut, BtnCol, Row, Btn);
    }
    return;
  case FIELD_BTN_EXIT:
    Row = Y + 18;
    {
      CHAR16 *Btn = L"[ Discard ]";
      UINTN BtnCol = X + BOX_WIDTH / 2 + 2;
      ConOut->SetAttribute(ConOut, IsSelected ? COLOR_BTN_HL : COLOR_BUTTON);
      TuiPrintAt(ConOut, BtnCol, Row, Btn);
    }
    return;
  default:
    return;
  }

  // Draw selection arrow
  ConOut->SetAttribute(ConOut, COLOR_ARROW);
  TuiPrintAt(ConOut, ArrowCol, Row, IsSelected ? L">" : L" ");

  // Draw label
  ConOut->SetAttribute(ConOut, IsSelected ? COLOR_TITLE : COLOR_LABEL);
  TuiPrintAt(ConOut, LabelCol, Row, Label);

  // Clear space between label and value
  {
    UINTN LabelEnd = LabelCol + StrLen(Label);
    CHAR16 Spaces[30];
    UINTN SpaceCount = 0;
    UINTN d;
    if (ValueCol > LabelEnd) {
      SpaceCount = ValueCol - LabelEnd;
      if (SpaceCount > 28) SpaceCount = 28;
      for (d = 0; d < SpaceCount; d++) {
        Spaces[d] = L' ';
      }
      Spaces[SpaceCount] = L'\0';
      ConOut->SetAttribute(ConOut, COLOR_LABEL);
      TuiPrintAt(ConOut, LabelEnd, Row, Spaces);
    }
  }

  // Draw value
  if (IsIpField && IpData != NULL) {
    Attr = IsSelected ? (IsEditing ? COLOR_EDITING : COLOR_HIGHLIGHT) : COLOR_VALUE;
    TuiDrawIpWithCursor(ConOut, ValueCol, Row, IpData, -1, Attr, Attr);
  } else {
    if (IsSelected && IsEditing) {
      Attr = COLOR_EDITING;
    } else if (IsSelected) {
      Attr = COLOR_HIGHLIGHT;
    } else {
      Attr = COLOR_VALUE;
    }
    ConOut->SetAttribute(ConOut, Attr);
    TuiPrintAt(ConOut, ValueCol, Row, Value);
  }
}

VOID
TuiDrawSectionHeaders(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y
  )
{
  UINTN LabelCol = X + 3;
  UINTN LineEnd = X + BOX_WIDTH - 5;
  UINTN i;
  CHAR16 Line[64];

  // "SERVER" section header
  ConOut->SetAttribute(ConOut, COLOR_SECTION);
  TuiPrintAt(ConOut, LabelCol, Y + 3, L"  SERVER");
  {
    UINTN Start = LabelCol + 9;
    UINTN Len = LineEnd - Start;
    if (Len > 62) Len = 62;
    for (i = 0; i < Len; i++) Line[i] = (CHAR16)BOX_H;
    Line[Len] = L'\0';
    ConOut->SetAttribute(ConOut, COLOR_BORDER);
    TuiPrintAt(ConOut, Start, Y + 3, Line);
  }

  // "NETWORK" section header
  ConOut->SetAttribute(ConOut, COLOR_SECTION);
  TuiPrintAt(ConOut, LabelCol, Y + 6, L"  NETWORK");
  {
    UINTN Start = LabelCol + 10;
    UINTN Len = LineEnd - Start;
    if (Len > 62) Len = 62;
    for (i = 0; i < Len; i++) Line[i] = (CHAR16)BOX_H;
    Line[Len] = L'\0';
    ConOut->SetAttribute(ConOut, COLOR_BORDER);
    TuiPrintAt(ConOut, Start, Y + 6, Line);
  }

  // "BOOT" section header
  ConOut->SetAttribute(ConOut, COLOR_SECTION);
  TuiPrintAt(ConOut, LabelCol, Y + 11, L"  BOOT");
  {
    UINTN Start = LabelCol + 7;
    UINTN Len = LineEnd - Start;
    if (Len > 62) Len = 62;
    for (i = 0; i < Len; i++) Line[i] = (CHAR16)BOX_H;
    Line[Len] = L'\0';
    ConOut->SetAttribute(ConOut, COLOR_BORDER);
    TuiPrintAt(ConOut, Start, Y + 11, Line);
  }
}

VOID
TuiDrawFooter(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y
  )
{
  UINTN FCol = X + 2;
  ConOut->SetCursorPosition(ConOut, FCol, Y + BOX_HEIGHT - 1);
  ConOut->SetAttribute(ConOut, COLOR_SECTION);
  ConOut->OutputString(ConOut, L" Up/Dn ");
  ConOut->SetAttribute(ConOut, COLOR_FOOTER);
  ConOut->OutputString(ConOut, L"Navigate  ");
  ConOut->SetAttribute(ConOut, COLOR_SECTION);
  ConOut->OutputString(ConOut, L"Enter ");
  ConOut->SetAttribute(ConOut, COLOR_FOOTER);
  ConOut->OutputString(ConOut, L"Edit  ");
  ConOut->SetAttribute(ConOut, COLOR_SECTION);
  ConOut->OutputString(ConOut, L"Tab ");
  ConOut->SetAttribute(ConOut, COLOR_FOOTER);
  ConOut->OutputString(ConOut, L"Next  ");
  ConOut->SetAttribute(ConOut, COLOR_SECTION);
  ConOut->OutputString(ConOut, L"Esc ");
  ConOut->SetAttribute(ConOut, COLOR_FOOTER);
  ConOut->OutputString(ConOut, L"Exit");
}

VOID
TuiDrawStatusLine(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y,
  IN CHAR16 *Message,
  IN UINTN Color
  )
{
  CHAR16 Blank[60];
  UINTN i;

  // Clear the status line area (row Y+16)
  for (i = 0; i < 56; i++) Blank[i] = L' ';
  Blank[56] = L'\0';

  ConOut->SetAttribute(ConOut, COLOR_LABEL);
  TuiPrintAt(ConOut, X + 2, Y + 16, Blank);

  if (Message != NULL) {
    ConOut->SetAttribute(ConOut, Color);
    UINTN MsgCol = X + (BOX_WIDTH - StrLen(Message)) / 2;
    TuiPrintAt(ConOut, MsgCol, Y + 16, Message);
  }
}

VOID
TuiDrawAll(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y,
  IN UINTN SelectedField,
  IN BOOLEAN IsEditing,
  IN HABOOT_CONFIG *Config
  )
{
  UINTN i;

  TuiDrawFrame(ConOut, X, Y);
  TuiDrawSectionHeaders(ConOut, X, Y);
  TuiDrawFooter(ConOut, X, Y);

  for (i = 0; i < FIELD_COUNT; i++) {
    TuiDrawField(ConOut, X, Y, i, SelectedField, IsEditing, Config);
  }
}

// IP octet editing helper
BOOLEAN
TuiEditIpField(
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn,
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y,
  IN UINTN FieldId,
  IN UINTN SelectedField,
  IN OUT UINT8 *IpOctets,
  IN HABOOT_CONFIG *Config
  )
{
  UINTN CurrentOctet = 0;
  CHAR16 NumBuf[4];
  UINTN NumPos;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;

  while (TRUE) {
    // Redraw field label + arrow
    TuiDrawField(ConOut, X, Y, FieldId, SelectedField, TRUE, Config);

    // Redraw IP with per-octet cursor highlight
    {
      UINTN ValueCol = X + 28;
      UINTN FieldRow;
      switch (FieldId) {
        case FIELD_SERVER_IP: FieldRow = Y + 4; break;
        case FIELD_STATIC_IP: FieldRow = Y + 8; break;
        case FIELD_SUBNET_MASK: FieldRow = Y + 9; break;
        case FIELD_GATEWAY: FieldRow = Y + 10; break;
        default: FieldRow = Y + 4; break;
      }
      TuiDrawIpWithCursor(ConOut, ValueCol, FieldRow, IpOctets, (INTN)CurrentOctet, COLOR_EDITING, COLOR_OCTET_CUR);
    }

    NumPos = 0;
    NumBuf[0] = L'\0';

    // Read digits for this octet
    while (TRUE) {
      Status = gBS->WaitForEvent(1, &ConIn->WaitForKey, NULL);
      if (EFI_ERROR(Status)) continue;

      Status = ConIn->ReadKeyStroke(ConIn, &Key);
      if (EFI_ERROR(Status)) continue;

      if (Key.ScanCode == SCAN_ESC) {
        return FALSE;  // Cancel editing
      }

      if (Key.ScanCode == SCAN_LEFT) {
        if (CurrentOctet > 0) CurrentOctet--;
        break;
      }
      if (Key.ScanCode == SCAN_RIGHT || Key.UnicodeChar == L'\t') {
        if (CurrentOctet < 3) CurrentOctet++;
        break;
      }

      if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
        // Apply any pending digits
        if (NumPos > 0) {
          NumBuf[NumPos] = L'\0';
          UINTN Val = 0;
          UINTN d;
          for (d = 0; d < NumPos; d++) {
            Val = Val * 10 + (NumBuf[d] - L'0');
          }
          if (Val > 255) Val = 255;
          IpOctets[CurrentOctet] = (UINT8)Val;
        }
        return TRUE;  // Confirm
      }

      if (Key.UnicodeChar >= L'0' && Key.UnicodeChar <= L'9' && NumPos < 3) {
        NumBuf[NumPos++] = Key.UnicodeChar;
        NumBuf[NumPos] = L'\0';

        // Apply immediately
        UINTN Val = 0;
        UINTN d;
        for (d = 0; d < NumPos; d++) {
          Val = Val * 10 + (NumBuf[d] - L'0');
        }
        if (Val > 255) Val = 255;
        IpOctets[CurrentOctet] = (UINT8)Val;

        TuiDrawField(ConOut, X, Y, FieldId, SelectedField, TRUE, Config);

        // Auto-advance after 3 digits
        if (NumPos >= 3) {
          if (CurrentOctet < 3) {
            CurrentOctet++;
          } else {
            return TRUE;
          }
          break;
        }
      }

      if (Key.UnicodeChar == CHAR_BACKSPACE && NumPos > 0) {
        NumPos--;
        NumBuf[NumPos] = L'\0';
        if (NumPos == 0) {
          IpOctets[CurrentOctet] = 0;
        } else {
          UINTN Val = 0;
          UINTN d;
          for (d = 0; d < NumPos; d++) {
            Val = Val * 10 + (NumBuf[d] - L'0');
          }
          IpOctets[CurrentOctet] = (UINT8)Val;
        }
        TuiDrawField(ConOut, X, Y, FieldId, SelectedField, TRUE, Config);
      }
    }
  }
}

// Port editing helper
BOOLEAN
TuiEditPortField(
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn,
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y,
  IN UINTN SelectedField,
  IN OUT UINT16 *Port,
  IN HABOOT_CONFIG *Config
  )
{
  CHAR16 NumBuf[6];
  UINTN NumPos = 0;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;

  while (TRUE) {
    TuiDrawField(ConOut, X, Y, FIELD_SERVER_PORT, SelectedField, TRUE, Config);

    Status = gBS->WaitForEvent(1, &ConIn->WaitForKey, NULL);
    if (EFI_ERROR(Status)) continue;

    Status = ConIn->ReadKeyStroke(ConIn, &Key);
    if (EFI_ERROR(Status)) continue;

    if (Key.ScanCode == SCAN_ESC) {
      return FALSE;
    }

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      return TRUE;
    }

    if (Key.UnicodeChar >= L'0' && Key.UnicodeChar <= L'9' && NumPos < 5) {
      NumBuf[NumPos++] = Key.UnicodeChar;
      NumBuf[NumPos] = L'\0';

      UINTN Val = 0;
      UINTN d;
      for (d = 0; d < NumPos; d++) {
        Val = Val * 10 + (NumBuf[d] - L'0');
      }
      if (Val > 65535) Val = 65535;
      *Port = (UINT16)Val;
      TuiDrawField(ConOut, X, Y, FIELD_SERVER_PORT, SelectedField, TRUE, Config);
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE && NumPos > 0) {
      NumPos--;
      if (NumPos == 0) {
        *Port = 0;
      } else {
        NumBuf[NumPos] = L'\0';
        UINTN Val = 0;
        UINTN d;
        for (d = 0; d < NumPos; d++) {
          Val = Val * 10 + (NumBuf[d] - L'0');
        }
        *Port = (UINT16)Val;
      }
      TuiDrawField(ConOut, X, Y, FIELD_SERVER_PORT, SelectedField, TRUE, Config);
    }
  }
}

// Timeout editing helper
BOOLEAN
TuiEditTimeoutField(
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn,
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y,
  IN UINTN SelectedField,
  IN OUT UINT16 *Timeout,
  IN HABOOT_CONFIG *Config
  )
{
  CHAR16 NumBuf[4];
  UINTN NumPos = 0;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;

  while (TRUE) {
    TuiDrawField(ConOut, X, Y, FIELD_BOOT_TIMEOUT, SelectedField, TRUE, Config);

    Status = gBS->WaitForEvent(1, &ConIn->WaitForKey, NULL);
    if (EFI_ERROR(Status)) continue;

    Status = ConIn->ReadKeyStroke(ConIn, &Key);
    if (EFI_ERROR(Status)) continue;

    if (Key.ScanCode == SCAN_ESC) return FALSE;

    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) return TRUE;

    if (Key.UnicodeChar >= L'0' && Key.UnicodeChar <= L'9' && NumPos < 3) {
      NumBuf[NumPos++] = Key.UnicodeChar;
      NumBuf[NumPos] = L'\0';

      UINTN Val = 0;
      UINTN d;
      for (d = 0; d < NumPos; d++) {
        Val = Val * 10 + (NumBuf[d] - L'0');
      }
      if (Val > 999) Val = 999;
      *Timeout = (UINT16)Val;
      TuiDrawField(ConOut, X, Y, FIELD_BOOT_TIMEOUT, SelectedField, TRUE, Config);
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE && NumPos > 0) {
      NumPos--;
      if (NumPos == 0) {
        *Timeout = 0;
      } else {
        NumBuf[NumPos] = L'\0';
        UINTN Val = 0;
        UINTN d;
        for (d = 0; d < NumPos; d++) {
          Val = Val * 10 + (NumBuf[d] - L'0');
        }
        *Timeout = (UINT16)Val;
      }
      TuiDrawField(ConOut, X, Y, FIELD_BOOT_TIMEOUT, SelectedField, TRUE, Config);
    }
  }
}

// Default OS editing helper - cycle through ALL known OS patterns + Menu
BOOLEAN
TuiEditDefaultOsField(
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn,
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN X,
  IN UINTN Y,
  IN UINTN SelectedField,
  IN OUT CHAR16 *DefaultOs,
  IN HABOOT_CONFIG *Config
  )
{
  // Build list from discovered OS entries (gOsList)
  UINTN i;
  UINTN CurrentIndex = 0;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;

  if (gOsCount == 0) return FALSE;

  // Find current selection index
  for (i = 0; i < gOsCount; i++) {
    if (gOsList[i].IsValid && StrCmpNoCase(gOsList[i].Name, DefaultOs) == 0) {
      CurrentIndex = i;
      break;
    }
  }

  while (TRUE) {
    TuiDrawField(ConOut, X, Y, FIELD_DEFAULT_OS, SelectedField, TRUE, Config);

    Status = gBS->WaitForEvent(1, &ConIn->WaitForKey, NULL);
    if (EFI_ERROR(Status)) continue;

    Status = ConIn->ReadKeyStroke(ConIn, &Key);
    if (EFI_ERROR(Status)) continue;

    if (Key.ScanCode == SCAN_ESC) return FALSE;
    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) return TRUE;

    if (Key.ScanCode == SCAN_LEFT || Key.ScanCode == SCAN_UP) {
      if (CurrentIndex > 0) {
        CurrentIndex--;
      } else {
        CurrentIndex = gOsCount - 1;
      }
      StrCpyS(DefaultOs, MAX_OS_NAME_LEN, gOsList[CurrentIndex].Name);
    }

    if (Key.ScanCode == SCAN_RIGHT || Key.ScanCode == SCAN_DOWN) {
      CurrentIndex++;
      if (CurrentIndex >= gOsCount) {
        CurrentIndex = 0;
      }
      StrCpyS(DefaultOs, MAX_OS_NAME_LEN, gOsList[CurrentIndex].Name);
    }
  }
}

VOID
RunSetupScreen(IN HABOOT_CONFIG *Config) {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut = gST->ConOut;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn = gST->ConIn;
  UINTN Cols, Rows;
  UINTN X, Y;
  UINTN SelectedField = FIELD_SERVER_IP;
  BOOLEAN Running = TRUE;
  HABOOT_CONFIG EditConfig;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;

  // Copy config for editing
  CopyMem(&EditConfig, Config, sizeof(HABOOT_CONFIG));

  // Get console size and compute centered position
  Status = ConOut->QueryMode(ConOut, ConOut->Mode->Mode, &Cols, &Rows);
  if (EFI_ERROR(Status)) {
    Cols = 80;
    Rows = 25;
  }

  X = (Cols > BOX_WIDTH) ? (Cols - BOX_WIDTH) / 2 : 0;
  Y = (Rows > (UINTN)(BOX_HEIGHT + 1)) ? (Rows - BOX_HEIGHT - 1) / 2 : 0;

  // Clear screen and hide cursor
  ConOut->ClearScreen(ConOut);
  ConOut->EnableCursor(ConOut, FALSE);

  // Initial draw
  TuiDrawAll(ConOut, X, Y, SelectedField, FALSE, &EditConfig);

  while (Running) {
    Status = gBS->WaitForEvent(1, &ConIn->WaitForKey, NULL);
    if (EFI_ERROR(Status)) continue;

    Status = ConIn->ReadKeyStroke(ConIn, &Key);
    if (EFI_ERROR(Status)) continue;

    // Navigation
    if (Key.ScanCode == SCAN_UP) {
      if (SelectedField > 0) {
        SelectedField--;
      }
      // Redraw all fields to update highlight
      UINTN i;
      for (i = 0; i < FIELD_COUNT; i++) {
        TuiDrawField(ConOut, X, Y, i, SelectedField, FALSE, &EditConfig);
      }
      continue;
    }

    if (Key.ScanCode == SCAN_DOWN) {
      if (SelectedField < FIELD_COUNT - 1) {
        SelectedField++;
      }
      UINTN i;
      for (i = 0; i < FIELD_COUNT; i++) {
        TuiDrawField(ConOut, X, Y, i, SelectedField, FALSE, &EditConfig);
      }
      continue;
    }

    // Tab to next field
    if (Key.UnicodeChar == L'\t') {
      SelectedField = (SelectedField + 1) % FIELD_COUNT;
      UINTN i;
      for (i = 0; i < FIELD_COUNT; i++) {
        TuiDrawField(ConOut, X, Y, i, SelectedField, FALSE, &EditConfig);
      }
      continue;
    }

    // Escape - exit without saving
    if (Key.ScanCode == SCAN_ESC) {
      Running = FALSE;
      continue;
    }

    // Enter - edit field or activate button
    if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      BOOLEAN Confirmed;

      switch (SelectedField) {
      case FIELD_SERVER_IP: {
        UINT8 Backup[4];
        CopyMem(Backup, EditConfig.ServerIp, 4);
        Confirmed = TuiEditIpField(ConIn, ConOut, X, Y, FIELD_SERVER_IP, SelectedField, EditConfig.ServerIp, &EditConfig);
        if (!Confirmed) CopyMem(EditConfig.ServerIp, Backup, 4);
        break;
      }
      case FIELD_SERVER_PORT: {
        UINT16 Backup = EditConfig.ServerPort;
        Confirmed = TuiEditPortField(ConIn, ConOut, X, Y, SelectedField, &EditConfig.ServerPort, &EditConfig);
        if (!Confirmed) EditConfig.ServerPort = Backup;
        break;
      }
      case FIELD_NET_MODE:
        // Toggle
        EditConfig.NetworkMode = (EditConfig.NetworkMode == NETWORK_MODE_STATIC) ?
                                  NETWORK_MODE_DHCP : NETWORK_MODE_STATIC;
        break;
      case FIELD_STATIC_IP: {
        UINT8 Backup[4];
        CopyMem(Backup, EditConfig.StaticIp, 4);
        Confirmed = TuiEditIpField(ConIn, ConOut, X, Y, FIELD_STATIC_IP, SelectedField, EditConfig.StaticIp, &EditConfig);
        if (!Confirmed) CopyMem(EditConfig.StaticIp, Backup, 4);
        break;
      }
      case FIELD_SUBNET_MASK: {
        UINT8 Backup[4];
        CopyMem(Backup, EditConfig.SubnetMask, 4);
        Confirmed = TuiEditIpField(ConIn, ConOut, X, Y, FIELD_SUBNET_MASK, SelectedField, EditConfig.SubnetMask, &EditConfig);
        if (!Confirmed) CopyMem(EditConfig.SubnetMask, Backup, 4);
        break;
      }
      case FIELD_GATEWAY: {
        UINT8 Backup[4];
        CopyMem(Backup, EditConfig.Gateway, 4);
        Confirmed = TuiEditIpField(ConIn, ConOut, X, Y, FIELD_GATEWAY, SelectedField, EditConfig.Gateway, &EditConfig);
        if (!Confirmed) CopyMem(EditConfig.Gateway, Backup, 4);
        break;
      }
      case FIELD_BOOT_TIMEOUT: {
        UINT16 Backup = EditConfig.BootTimeout;
        Confirmed = TuiEditTimeoutField(ConIn, ConOut, X, Y, SelectedField, &EditConfig.BootTimeout, &EditConfig);
        if (!Confirmed) EditConfig.BootTimeout = Backup;
        break;
      }
      case FIELD_DEFAULT_OS: {
        CHAR16 Backup[MAX_OS_NAME_LEN];
        StrCpyS(Backup, MAX_OS_NAME_LEN, EditConfig.DefaultOs);
        Confirmed = TuiEditDefaultOsField(ConIn, ConOut, X, Y, SelectedField, EditConfig.DefaultOs, &EditConfig);
        if (!Confirmed) StrCpyS(EditConfig.DefaultOs, MAX_OS_NAME_LEN, Backup);
        break;
      }
      case FIELD_BTN_TEST: {
        TuiDrawStatusLine(ConOut, X, Y, L"Testing connection...", COLOR_LABEL);
        Status = TestConnection(&EditConfig);
        if (!EFI_ERROR(Status)) {
          TuiDrawStatusLine(ConOut, X, Y, L"Connection successful!", COLOR_STATUS_OK);
        } else {
          TuiDrawStatusLine(ConOut, X, Y, L"Connection failed!", COLOR_STATUS_ERR);
        }
        break;
      }
      case FIELD_BTN_SAVE:
        CopyMem(Config, &EditConfig, sizeof(HABOOT_CONFIG));
        SaveConfig(Config);
        Running = FALSE;
        break;
      case FIELD_BTN_EXIT:
        Running = FALSE;
        break;
      }

      // Redraw fields
      {
        UINTN i;
        for (i = 0; i < FIELD_COUNT; i++) {
          TuiDrawField(ConOut, X, Y, i, SelectedField, FALSE, &EditConfig);
        }
      }
    }

    // Left/Right on enum field (NetworkMode) - toggle without Enter
    if (SelectedField == FIELD_NET_MODE &&
        (Key.ScanCode == SCAN_LEFT || Key.ScanCode == SCAN_RIGHT)) {
      EditConfig.NetworkMode = (EditConfig.NetworkMode == NETWORK_MODE_STATIC) ?
                                NETWORK_MODE_DHCP : NETWORK_MODE_STATIC;
      TuiDrawField(ConOut, X, Y, FIELD_NET_MODE, SelectedField, FALSE, &EditConfig);
    }
  }

  // Restore screen
  ConOut->SetAttribute(ConOut, COLOR_NORMAL);
  ConOut->ClearScreen(ConOut);
  ConOut->EnableCursor(ConOut, TRUE);
}

// ============================================================================
// Setup Key Detection
// ============================================================================

VOID
SplashDrawBorder(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut,
  IN UINTN Cols,
  IN UINTN Rows,
  IN UINTN Attr
  )
{
  CHAR16 Buf[256];
  UINTN Row, i;
  UINTN W = Cols;

  if (W > 254) W = 254;

  ConOut->SetAttribute(ConOut, Attr);

  // Top
  Buf[0] = (CHAR16)BOX2_TL;
  for (i = 1; i < W - 1; i++) Buf[i] = (CHAR16)BOX2_H;
  Buf[W - 1] = (CHAR16)BOX2_TR;
  Buf[W] = L'\0';
  ConOut->SetCursorPosition(ConOut, 0, 0);
  ConOut->OutputString(ConOut, Buf);

  // Sides
  Buf[0] = (CHAR16)BOX2_V;
  for (i = 1; i < W - 1; i++) Buf[i] = L' ';
  Buf[W - 1] = (CHAR16)BOX2_V;
  Buf[W] = L'\0';
  for (Row = 1; Row < Rows - 1; Row++) {
    ConOut->SetCursorPosition(ConOut, 0, Row);
    ConOut->SetAttribute(ConOut, Attr);
    ConOut->OutputString(ConOut, Buf);
  }

  // Bottom
  Buf[0] = (CHAR16)BOX2_BL;
  for (i = 1; i < W - 1; i++) Buf[i] = (CHAR16)BOX2_H;
  Buf[W - 1] = (CHAR16)BOX2_BR;
  Buf[W] = L'\0';
  ConOut->SetCursorPosition(ConOut, 0, Rows - 1);
  ConOut->OutputString(ConOut, Buf);
}

BOOLEAN
CheckForSetupKey(IN UINT16 TimeoutSeconds) {
  EFI_STATUS Status;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut = gST->ConOut;
  UINTN Cols, Rows;
  UINTN Countdown;
  UINTN CenterRow, CenterCol;
  CHAR16 Msg[80];
  CHAR16 CountMsg[20];
  BOOLEAN SetupPressed = FALSE;

  Status = ConOut->QueryMode(ConOut, ConOut->Mode->Mode, &Cols, &Rows);
  if (EFI_ERROR(Status)) {
    Cols = 80;
    Rows = 25;
  }

  ConOut->ClearScreen(ConOut);
  ConOut->EnableCursor(ConOut, FALSE);

  CenterRow = Rows / 2;
  CenterCol = Cols / 2;

  // Draw title
  {
    CHAR16 *Title = L"HA BOOT MANAGER";
    UINTN TitleCol = CenterCol - StrLen(Title) / 2;
    ConOut->SetAttribute(ConOut, COLOR_SPLASH_TXT);
    TuiPrintAt(ConOut, TitleCol, CenterRow - 2, Title);
  }

  for (Countdown = TimeoutSeconds; Countdown > 0; Countdown--) {
    // Draw countdown
    UnicodeSPrint(Msg, sizeof(Msg), L"Press ");
    ConOut->SetAttribute(ConOut, COLOR_SPLASH_TXT);
    TuiPrintAt(ConOut, CenterCol - 8, CenterRow, Msg);
    ConOut->SetAttribute(ConOut, COLOR_SPLASH_KEY);
    ConOut->OutputString(ConOut, L"S");
    ConOut->SetAttribute(ConOut, COLOR_SPLASH_TXT);
    ConOut->OutputString(ConOut, L" for Setup");

    UnicodeSPrint(CountMsg, sizeof(CountMsg), L"%d ", Countdown);
    ConOut->SetAttribute(ConOut, COLOR_SPLASH_KEY);
    TuiPrintAt(ConOut, CenterCol - 1, CenterRow + 2, CountMsg);

    // Poll for key press over 1 second (10 x 100ms)
    UINTN Poll;
    for (Poll = 0; Poll < 10; Poll++) {
      EFI_INPUT_KEY Key;
      Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
      if (!EFI_ERROR(Status)) {
        if (Key.UnicodeChar == L's' || Key.UnicodeChar == L'S') {
          SetupPressed = TRUE;
          break;
        }
      }
      gBS->Stall(100000);  // 100ms
    }

    if (SetupPressed) break;
  }

  ConOut->SetAttribute(ConOut, COLOR_NORMAL);
  ConOut->ClearScreen(ConOut);

  return SetupPressed;
}

// ============================================================================
// Main Entry Point
// ============================================================================

EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  OS_ENTRY *SelectedOs = NULL;
  HABOOT_CONFIG Config;

  // Phase 0: Set max console text mode, then max GOP resolution
  {
    // Set max console text mode first
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut = gST->ConOut;
    INT32 BestMode = 0;
    UINTN BestCols = 0, BestRows = 0;
    INT32 TxtMode;
    for (TxtMode = 0; TxtMode < ConOut->Mode->MaxMode; TxtMode++) {
      UINTN Cols, Rows;
      if (!EFI_ERROR(ConOut->QueryMode(ConOut, TxtMode, &Cols, &Rows))) {
        if (Cols * Rows > BestCols * BestRows) {
          BestCols = Cols;
          BestRows = Rows;
          BestMode = TxtMode;
        }
      }
    }
    if (BestMode != ConOut->Mode->Mode) {
      ConOut->SetMode(ConOut, BestMode);
    }

    // Then set max GOP resolution (after text mode, so it takes priority)
    SetMaxGopResolution();
  }

  // Phase 1: Load config from NVRAM (or defaults)
  LoadConfig(&Config);

  // Phase 2: Scan ESP for bootable OS entries
  ScanBootableOS();

  // If no OS found, exit to next boot manager
  if (gOsCount <= 1) {  // Only "Menu" entry
    return EFI_SUCCESS;
  }

  // Phase 3: Check for 's' setup key
  if (CheckForSetupKey(Config.BootTimeout)) {
    RunSetupScreen(&Config);
  }

  // Phase 4: Load network drivers and connect
  LoadNetworkDrivers(ImageHandle);
  ConnectAllControllers();

  // Phase 5: Configure network
  Status = WaitForNetworkWithConfig(&Config);
  if (EFI_ERROR(Status)) {
    // Network failed, try booting default OS
    BootDefaultOs(ImageHandle, &Config);
    return EFI_SUCCESS;
  }

  // Phase 6: Query TCP server
  Status = QueryTcpServerWithConfig(&Config, &SelectedOs);
  if (EFI_ERROR(Status) || SelectedOs == NULL) {
    // Server query failed, try default OS
    BootDefaultOs(ImageHandle, &Config);
    return EFI_SUCCESS;
  }

  // Phase 7: Handle selection
  if (StrCmp(SelectedOs->Path, L"MENU") == 0) {
    return EFI_SUCCESS;
  }

  // Boot selected OS
  Status = BootEfiFile(ImageHandle, SelectedOs->Path);
  if (EFI_ERROR(Status)) {
    // Boot failed, try default OS as fallback
    BootDefaultOs(ImageHandle, &Config);
  }

  return EFI_SUCCESS;
}
