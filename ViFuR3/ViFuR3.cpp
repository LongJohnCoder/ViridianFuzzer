/*++

Module Name:

    ViFuR3.cpp

Abstract:

    Usermode component of the Viridian Fuzzer. Sends IOCTLs to kernel driver
    to run CPUID, read/write MSRs, and make hypercalls. 

Authors:

    Amardeep Chana

Environment:

    User mode

--*/

#include "stdafx.h"
#include "ViFuR3.h"

HANDLE g_hLogfile = NULL;
HANDLE g_hFuzzLogger = NULL;

//
// Append data to the end of the UNC path file, and print to stdout
//
VOID 
WriteToLogFile (
    IN HANDLE       hFile,
    IN const CHAR   *fmt, 
    IN ...
)
{
    CHAR        buffer[4096];
    va_list     args = NULL;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    
    WriteFile(hFile, buffer, (DWORD)strlen(buffer), NULL, NULL);

    va_end(args);
}

//
// Reads the log file and extracts the fuzz entry if found
//
VOID
GetLastEntryFromFuzzLog (
    IN OUT PUSHORT     pCallcode, 
    IN OUT PUSHORT     pIsRepCnt,
    IN OUT PUSHORT     pIsFast,
    IN OUT PUSHORT     pI
)
{
    CONST INT   bytesToRead = 100;
    CHAR        buf[bytesToRead + 1] = { 0 };
    DWORD       bytesRead = 0;
    DWORD       status = -1;
    DWORD       endLastline = 0;
    DWORD       startLastline = 0;
    USHORT      prevCallcode = 0;
    USHORT      prevIsRepCnt = 0;
    USHORT      prevIsFast = 0;
    USHORT      prevCase = 0;

    status = SetFilePointer(g_hFuzzLogger, -bytesToRead, NULL, FILE_END);
    if (status == INVALID_SET_FILE_POINTER)
    {
        printf("[-] SetFilePointer %ws, %x\n", UNC_LOG_FUZZCMD, GetLastError());
    }

    status = ReadFile(g_hFuzzLogger, &buf, bytesToRead, &bytesRead, NULL);
    if (!status)
    {
        printf("[-] ReadFile, %x\n", GetLastError());
    }

    //
    // Find a "]\r\n" from "[ date ]\r\n" as its stored every crash
    //
    for (INT i = bytesToRead; i > 0; i--)
    {
        if (buf[i - 2] != ']' && buf[i - 1] == '\r' && buf[i] == '\n')
        {
            endLastline = i-1;
            break;
        }
    }
    for (INT i = endLastline - 2; i > 0; i--)
    {
        if (buf[i - 2] == '\r' && buf[i - 1] == '\n' && buf[i - 2] != '[')
        {
            startLastline = i;
            break;
        }
    }

    //
    // Once date line found, extract the last entry of fuzzer
    //
    sscanf_s(&buf[startLastline], 
            "%hx %hx %hx %hx", 
            &prevCallcode, 
            &prevIsRepCnt, 
            &prevIsFast, 
            &prevCase);

    //
    // Basic validation of fuzz entry
    //
    if ((prevCallcode >= 0 && prevCallcode < 0xFFF) &&
        (prevIsRepCnt >= 0 && prevIsRepCnt < 0xFF) &&
        (prevIsFast == 0 || prevIsFast == 1) &&
        (prevCase >= 0 || prevCase < 0xFFF)
        )
    {
        printf("[+] Last entry in fuzzer was: %x %x %x %x\n",
                prevCallcode,
                prevIsRepCnt,
                prevIsFast,
                prevCase);

        *pCallcode = prevCallcode;
        *pIsRepCnt = prevIsRepCnt;
        *pIsFast = prevIsFast;
        *pI = prevCase;
    }
    else
    {
        printf("[-] ERR Corrupted entry in fuzz cmd file: %x %x %x %x\n",
                prevCallcode,
                prevIsRepCnt,
                prevIsFast,
                prevCase);
        exit(-5);
    }
}

//
// Execute a CPUID - will pass params to the kernel component
//
VOID 
ExecCpuid (
    IN  HANDLE      hDevice,
    IN  INT         cpuid, 
    OUT PCPU_REG_32 pOutReg
)
{
    BOOL    bStatus = FALSE;
    DWORD   bytesRet = 0;

    //
    // On success output parameter pOutReg is filled in from driver
    //
    bStatus = DeviceIoControl(hDevice,
                              IOCTL_CPUID,
                              &cpuid,
                              sizeof(INT),
                              pOutReg,
                              sizeof(PCPU_REG_32),
                              &bytesRet,
                              NULL);

    if (!bStatus || bytesRet != sizeof(CPU_REG_32))
    {
        printf("[-] ERR DeviceIoControl IOCTL_CPUID 0x%x\r\n", GetLastError());
    }
}

//
// Execute a read or write to an MSR
//
VOID 
ExecMsr (
    IN  HANDLE  hDevice,
    IN  ULONG   msr,
    IN  DWORD   readOrWrite,
    OUT PULONG  pMsrData
)
{
    BOOL    bStatus = FALSE;
    DWORD   bytesRet = 0;
    DWORD   outputBuf = NULL;

    if (readOrWrite == MSR_R)
    {
        bStatus = DeviceIoControl(hDevice,
                                  IOCTL_MSR_READ,
                                  &msr,
                                  4,
                                  &outputBuf,
                                  4,
                                  &bytesRet,
                                  NULL);

        if (!bStatus && bytesRet != 4)
        {
            printf("[-] ERR DeviceIoControl 0x%x\r\n", GetLastError());
        }
        *pMsrData = outputBuf;
    }
}

//
// Execute a hypercall
//
UINT32
ExecHypercall (
    IN  HANDLE      hDevice,
    IN  PCPU_REG_64 pInputBuf,
    IN  DWORD       inputBufLen,
    OUT PVOID       pOutputBuf,
    IN  DWORD       outputBufLen,
    OUT PDWORD      pBytesRet
)
{
    BOOL    bStatus = FALSE;
    DWORD   bytesRet = 0;
    UINT32  hvErr = HV_STATUS_NO_DATA;

    bStatus = DeviceIoControl(hDevice,
                              IOCTL_HYPERCALL,
                              pInputBuf,
                              sizeof(CPU_REG_64),
                              pOutputBuf,
                              outputBufLen,
                              &bytesRet,
                              NULL);

    if (!bStatus)
    {
        //
        // VirdianFuzzer driver returns a custom error code if
        // it set it that can be obtained from GLE
        //
        hvErr = GetLastError();
        if (IS_VIFU_ERR(hvErr))
        {
            if (VIFU_ERR_FACILITY(hvErr) == FACILITY_HYPERV)
            {
                WriteToLogFile(g_hLogfile,
                               "[-] ERR DeviceIoControl - HyperV 0x%x\r\n", 
                               VIFU_ERR_CODE(hvErr));
                
            }
            else if (VIFU_ERR_FACILITY(hvErr) == FACILITY_VIFU)
            {
                WriteToLogFile(g_hLogfile,
                               "[-] ERR DeviceIoControl - ViFu 0x%x\r\n", 
                               VIFU_ERR_CODE(hvErr));
            }
        }
        else
        {
            WriteToLogFile(g_hLogfile,
                           "[-] ERR DeviceIoControl 0x%x\r\n",
                           GetLastError());
        }
    }
    else
    {
        hvErr = HV_STATUS_SUCCESS;
        *pBytesRet = bytesRet;
    }
    
    return hvErr;
}

INT 
main (
    IN INT      argc, 
    IN PCHAR    argv[]
)
{
    HANDLE      hDevice = NULL;
    DWORD       status = ERROR;
    UINT32      hvStatus = FALSE;
    DWORD       bytesRet = 0;
    PDWORD      pOutputBuf = NULL;
    DWORD       cntCallsSuccess = 0;
    UINT64      uniqueCalls[0xfff] = { 0 };
    DWORD       cntUniqueCalls = 0;

    //
    // Only start if autoStart.txt file exists on the UNC share
    //
    DWORD dwAttrib = GetFileAttributes(AUTO_START_FILE);

    if (dwAttrib == INVALID_FILE_ATTRIBUTES)
    {
        printf("[-] Auto start file not found [%ws], ERR 0x%x \r\n", 
                AUTO_START_FILE, 
                GetLastError());
        exit(-1);
    }

    //
    // Open handle to our driver
    //
    hDevice = CreateFile(DRIVER_WIN_OBJ, 
                         GENERIC_WRITE | GENERIC_READ, 
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL, 
                         OPEN_EXISTING, 
                         FILE_ATTRIBUTE_NORMAL, 
                         NULL);

    if (hDevice == INVALID_HANDLE_VALUE)
    {
        printf("[-] ERR CreateFile %d\r\n", GetLastError());
        return -1;
    }

    //
    // IOCTL test
    //
    pOutputBuf = (PDWORD)calloc(4, 1);
    hvStatus = DeviceIoControl(hDevice, 
                              IOCTL_HELLO, 
                              NULL, 
                              0, 
                              pOutputBuf, 
                              4, 
                              &bytesRet, 
                              NULL);
    if (!hvStatus)
    {
        printf("[-] ERR DeviceIoControl (custom?) %d\r\n", GetLastError());
        exit(-11);
    }
    if (*pOutputBuf == 0x41424344)
    {
        printf("[+] IOCTL comms operating succesfully\n");
    }
    free(pOutputBuf);

    //
    // CPUID - Get Vendor ID
    //
    CPU_REG_32 outReg = { 0 };
    DWORD cpuId = 0x00000000;
    ExecCpuid(hDevice, cpuId, &outReg);
    printf("[+] CPUID [0x%08x] = ", cpuId);
    PRINT_CPU_REG(outReg.eax, outReg.ebx, outReg.ecx, outReg.edx);
    printf("[!] Vendor ID: %.4s%.4s%.4s\r\n", 
           (CHAR*)&outReg.ebx, 
           (CHAR*)&outReg.edx, 
           (CHAR*)&outReg.ecx);
    
    //
    // MSR - Read, Reference timer
    //
    DWORD msrData = 0;
    DWORD msr = 0x40000020;
    ExecMsr(hDevice, msr, MSR_R, &msrData);
    printf("[+] MSR [0x%08x] = 0x%08x\r\n", msr, msrData);
    
    //
    // Hypercall - Fuzz
    //    
    bytesRet = 0;

    //
    // Connect to UNC share (requires target user creds in Credential Manager)
    //
    NETRESOURCE networkRes = { 0 };
    networkRes.dwType = RESOURCETYPE_ANY;
    networkRes.lpLocalName = NULL;
    networkRes.lpRemoteName = (PWCHAR)UNC_LOG_PATH;
    networkRes.lpProvider = NULL;
    DWORD ret = WNetAddConnection3(NULL, 
                                   &networkRes, 
                                   NULL, 
                                   NULL, 
                                   CONNECT_INTERACTIVE);

    g_hLogfile = CreateFile(UNC_LOG_FILEPATH,
                            FILE_APPEND_DATA,
                            NULL,
                            NULL,
                            OPEN_EXISTING,
                            FILE_FLAG_WRITE_THROUGH,
                            NULL);

    g_hFuzzLogger = CreateFile(UNC_LOG_FUZZCMD,
                               GENERIC_READ|FILE_APPEND_DATA,
                               NULL,
                               NULL,
                               OPEN_EXISTING,
                               FILE_FLAG_WRITE_THROUGH,
                               NULL);
                                
    if (g_hLogfile == INVALID_HANDLE_VALUE)
    {
        printf("[-] ERR opening UNC share VifuLog, %x\n", GetLastError());
        exit(-12);
    } 

    if (g_hFuzzLogger == INVALID_HANDLE_VALUE)
    {
        printf("[-] ERR opening UNC share FuzzCmd, %x\n", GetLastError());
        exit(-13);
    }
    
    //
    // Check if we are resuming from previous crash
    //
    BOOL bResume = FALSE;
    if (GetFileSize(g_hFuzzLogger, NULL) > 0)
    {
        bResume = TRUE;
    }
    else
    {
        printf("[+] Starting fuzzer fresh\n");
        bResume = FALSE;
    }

    SYSTEMTIME st = { 0 };
    GetLocalTime(&st);
    WriteToLogFile(g_hLogfile, 
                   STR_FMT_DATETIME, 
                   st.wDay, 
                   st.wMonth, 
                   st.wYear, 
                   st.wHour, 
                   st.wMinute, 
                   st.wSecond);

    WriteToLogFile(g_hFuzzLogger, 
                   STR_FMT_DATETIME, 
                   st.wDay, 
                   st.wMonth, 
                   st.wYear, 
                   st.wHour, 
                   st.wMinute, 
                   st.wSecond);

    //
    // If VIFU ran in root, these cause BSODS
    //
    // const WORD BSOD_CALLCODES[] = {0x00, 0x01, 0x11, 0x12, 0x0a, 0x4,0x76,0x53,0x6b,0x7a,0x7b,0x7c,0x86};

    //
    // Child BSODS
    //
    const WORD BSOD_CALLCODES[] = { 0x01, 0x0a, 0x11, 0x12 };

    //
    // BASIC FUZZER LOOPS (: 
    //
    // Iterate all callcodes, then flip rep bit, then fast bit, then specific conditions
    //
    CPU_REG_64 regsOut = { 0 };
    CPU_REG_64 inRegs = { 0 };
    HV_X64_HYPERCALL_INPUT hvCallInput = { 0 };

    for (USHORT callcode = 0x0; callcode < _ARRAYSIZE(HypercallEntries); callcode++)
    {
        for (USHORT isRepCnt = 0; isRepCnt <= 2; isRepCnt++)
        {
            for (USHORT isFast = 0; isFast <= 1; isFast++)
            {
                printf( "Hypercall: 0x%llx\r\n", hvCallInput.AsUINT64 );

                //
                // Loop the switch() statement to set regs
                //
                for (USHORT i = 0; i <= 8+64+64; i++)
                {
                    if (bResume)
                    {
                        bResume = FALSE;
                        GetLastEntryFromFuzzLog(&callcode, &isRepCnt, &isFast, &i);
                        //
                        // If previous fuzz log entry found, skip all the i's
                        //
                        goto HC_END_CURRENT_ISFAST;
                    }
                   
                    //
                    // Avoid "HvReserved" and BAD_CALLCODES as they usually BSOD
                    //
                    if (strstr(HypercallEntries[callcode].name, "Reserved") != 0)
                    {
                        printf("[!] Avoiding hypercall %x\r\n", callcode);
                        goto HC_END_CURRENT_CALLCODE;
                    }

                    for (int b = 0; b < _ARRAYSIZE(BSOD_CALLCODES); b++)
                    {
                        if (BSOD_CALLCODES[b] == callcode)
                        {
                            printf("[!] Avoiding hypercall %x\r\n", callcode);
                            goto HC_END_CURRENT_CALLCODE;
                        }
                    }           

                    //
                    // Set fuzz regs
                    //
                    regsOut = { 0 };
                    ZeroMemory( &inRegs, sizeof( CPU_REG_64 ) );
                    hvCallInput.AsUINT64 = 0;

                    hvCallInput.callCode = callcode;
                    hvCallInput.fastCall = isFast;
                    hvCallInput.repCnt = isRepCnt;
                    inRegs.rcx = hvCallInput.AsUINT64;

                    switch (i)
                    {
                        //
                        // Extended tests
                        //
                    case 120:
                        inRegs.xmm0.lower = 0x0DCDCDCDCDCDCDCD;
                        inRegs.xmm0.upper = 0x0FEFEFEFEFEFEFEF;
                        break;
                    case 121: 
                        if( isFast == 1 )
                        {
                            inRegs.xmm0.lower = 0; inRegs.xmm0.upper = 0;
                            inRegs.xmm1.lower = 0; inRegs.xmm1.upper = 0;
                            inRegs.xmm2.lower = 0; inRegs.xmm2.upper = 0;
                        }
                        break;
                    case 122: 
                        if( isFast == 1 )
                        {
                            inRegs.xmm0.lower = 1; inRegs.xmm0.upper = 0;
                            inRegs.xmm1.lower = 1; inRegs.xmm1.upper = 0;
                            inRegs.xmm2.lower = 0; inRegs.xmm2.upper = 0;
                        }
                        break;
                    case 123:
                        if( isFast == 1 )
                        {
                            inRegs.xmm0.lower = 1; inRegs.xmm0.upper = 0;
                            inRegs.xmm1.lower = 1; inRegs.xmm1.upper = 0;
                            inRegs.xmm2.lower = 1; inRegs.xmm2.upper = 0;
                        }
                        break;
                        //
                        // Generate case ranges from the python script create_cases.py
                        //
                    case 72: case 73: case 74: case 75: case 76: case 77:
                    case 78: case 79: case 80: case 81: case 82: case 83:
                    case 84: case 85: case 86: case 87: case 88: case 89:
                    case 90: case 91: case 92: case 93: case 94: case 95:
                    case 96: case 97: case 98: case 99: case 100: case 101:
                    case 102: case 103: case 104: case 105: case 106: case 107:
                    case 108: case 109: case 110: case 111: case 112: case 113:
                    case 114: case 115: case 116: case 117: case 118: case 119:
                    case 124: case 125:
                    case 126: case 127: case 128: case 129: case 130: case 131:
                    case 132: case 133: case 134: case 135:
                        //
                        // Set bits from 0-64, replicating bitmasks etc in hV
                        // RAX used in driver to set *GPA content
                        //
                        inRegs.rdx = USE_GPA_MEM_BIT_RANGE_LOOP;
                        inRegs.r8 = USE_GPA_MEM_BIT_RANGE_LOOP;
                        inRegs.rax = 0ULL | (1ULL << (i - 72));
                        break;

                    case 8: case 9: case 10: case 11: case 12: case 13:
                    case 14: case 15: case 16: case 17: case 18: case 19:
                    case 20: case 21: case 22: case 23: case 24: case 25:
                    case 26: case 27: case 28: case 29: case 30: case 31:
                    case 32: case 33: case 34: case 35: case 36: case 37:
                    case 38: case 39: case 40: case 41: case 42: case 43:
                    case 44: case 45: case 46: case 47: case 48: case 49:
                    case 50: case 51: case 52: case 53: case 54: case 55:
                    case 56: case 57: case 58: case 59: case 60: case 61:
                    case 62: case 63: case 64: case 65: case 66: case 67:
                    case 68: case 69: case 70: case 71:
                        //
                        // Only 1 arg in with GPA, bits set
                        //
                        inRegs.rdx = USE_GPA_MEM_BIT_RANGE_LOOP;
                        inRegs.r8 = 0;
                        inRegs.rax = 0ULL | (1ULL << (i - 8));
                        break;

                    case 7:
                        //
                        // Fill GPA NonPagedPool mem with 1's
                        //
                        inRegs.rdx = USE_GPA_MEM_NOFILL_1;
                        break;
                    case 6:
                        inRegs.rdx = USE_GPA_MEM_NOFILL_0;
                        break;
                    case 5:
                        //
                        // No in/out args
                        //
                        inRegs.r8 = 0;
                        inRegs.rdx = 0;
                        break;
                    case 4:
                        //
                        // Intentionally fall through cases and set other regs
                        // USE_GPA_MEM_FILL is replaced in driver with non paged pool ptrs
                        //
                        inRegs.r11 = USE_GPA_MEM_FILL;
                    case 3:
                        inRegs.r10 = USE_GPA_MEM_FILL;
                    case 2:
                        inRegs.r9 = USE_GPA_MEM_FILL;
                    case 1:
                        inRegs.r8 = USE_GPA_MEM_FILL;
                    case 0:
                        inRegs.rdx = USE_GPA_MEM_FILL;
                        break;
                    default:
                        break;
                    }

                    // printf("0x%llx 0x%llx\r\n", hvCallInput.AsUINT64, inRegs.rax);

                    WriteToLogFile(g_hFuzzLogger, 
                                   "%x %x %x %x\r\n", 
                                   callcode, 
                                   isRepCnt, 
                                   isFast, 
                                   i);

                    WriteToLogFile(g_hLogfile, 
                                   "[ ] %s [0x%llx]\r\n", 
                                   HypercallEntries[callcode].name, 
                                   hvCallInput.AsUINT64);

                    WRITE_REGS_TO_LOG_FILE(inRegs.rax, 
                                           inRegs.rbx, 
                                           inRegs.rcx, 
                                           inRegs.rdx, 
                                           inRegs.rsi, 
                                           inRegs.rdi, 
                                           inRegs.r8, 
                                           inRegs.r9, 
                                           inRegs.r10, 
                                           inRegs.r11);

                    hvStatus = ExecHypercall(hDevice,
                                             &inRegs,
                                             sizeof(CPU_REG_64),
                                             &regsOut,
                                             sizeof(CPU_REG_64),
                                             &bytesRet);

                    if (hvStatus == HV_STATUS_SUCCESS)
                    {
                        BOOL isUnique = TRUE;
                        for (DWORD u = 0; u < cntUniqueCalls; u++)
                        {
                            if (uniqueCalls[u] == inRegs.rcx)
                            {
                                isUnique = FALSE;
                                break;
                            }
                        }
                        if (isUnique)
                        {
                            uniqueCalls[cntUniqueCalls] = inRegs.rcx;
                            cntUniqueCalls++;
                        }

                        cntCallsSuccess++;
                        WriteToLogFile(g_hLogfile, "[+] Success\n");
                        printf("[+]\n");
                        WRITE_REGS_TO_LOG_FILE(regsOut.rax, regsOut.rbx, regsOut.rcx, regsOut.rdx, regsOut.rsi, regsOut.rdi, regsOut.r8, regsOut.r9, regsOut.r10, regsOut.r11);
                    }

                HC_END_CURRENT_I:
                    continue;
                }
            HC_END_CURRENT_ISFAST:
                continue;
                //exit(-99);    //# test - 1 HC switch #
            }
        }
        //exit(-999);    //# test - all of hypercalls
    HC_END_CURRENT_CALLCODE:
        continue;
    }

    printf("[+] Calls made: %d\r\n", cntCallsSuccess);
    printf("[+] Unique calls: %d\r\n", cntUniqueCalls);
    WriteToLogFile(g_hLogfile, "[+] Calls made: %d\r\n", cntCallsSuccess);
    WriteToLogFile(g_hLogfile, "[+] Unique calls: %d\r\n", cntUniqueCalls);
    for (DWORD u = 0; u < cntUniqueCalls; u++)
    {
        WriteToLogFile(g_hLogfile, "      0x%llx \r\n", uniqueCalls[u]);
        printf("      0x%llx \r\n", uniqueCalls[u]);
    }

    CloseHandle(g_hLogfile);
    CloseHandle(g_hFuzzLogger);

    return 0;
}
