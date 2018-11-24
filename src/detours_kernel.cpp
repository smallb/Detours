//////////////////////////////////////////////////////////////////////////////
//
//  Core Detours Functionality (detours_kernel.cpp of detours.lib)
//
//  Microsoft Research Detours Package, Version 4.0.1
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//

#include "internal.h"


#ifdef DetoursKernelMode

//////////////////////////////////////////////////////////////////////////////
//
struct _DETOUR_ALIGN
{
    BYTE    obTarget : 3;
    BYTE    obTrampoline : 5;
};

C_ASSERT(sizeof(_DETOUR_ALIGN) == 1);

//////////////////////////////////////////////////////////////////////////////
//
// Region reserved for system DLLs, which cannot be used for trampolines.
//
static PVOID    s_pSystemRegionLowerBound = (PVOID)(ULONG_PTR)0x70000000;
static PVOID    s_pSystemRegionUpperBound = (PVOID)(ULONG_PTR)0x80000000;

//////////////////////////////////////////////////////////////////////////////
//
static bool detour_is_imported(PBYTE pbCode, PBYTE pbAddress)
{
    MEMORY_BASIC_INFORMATION mbi;
    DetoursQueryModuleMemoryBaseInformationForAddress((PVOID)pbCode, &mbi, sizeof(mbi));
    __try
    {
        PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)mbi.AllocationBase;
        if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return false;
        }

        PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((PBYTE)pDosHeader +
            pDosHeader->e_lfanew);
        if (pNtHeader->Signature != IMAGE_NT_SIGNATURE)
        {
            return false;
        }

        if (pbAddress >= ((PBYTE)pDosHeader +
            pNtHeader->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress) &&
            pbAddress < ((PBYTE)pDosHeader +
                pNtHeader->OptionalHeader
                .DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress +
                pNtHeader->OptionalHeader
                .DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size))
        {
            return true;
        }
    }
#pragma prefast(suppress:28940, "A bad pointer means this probably isn't a PE header.")
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
        EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }

    return false;
}

inline ULONG_PTR detour_2gb_below(ULONG_PTR address)
{
    return (address > (ULONG_PTR)0x7ff80000) ? address - 0x7ff80000 : 0x80000;
}

inline ULONG_PTR detour_2gb_above(ULONG_PTR address)
{
#if defined(DETOURS_64BIT)
    return (address < (ULONG_PTR)0xffffffff80000000) ? address + 0x7ff80000 : (ULONG_PTR)0xfffffffffff80000;
#else
    return (address < (ULONG_PTR)0x80000000) ? address + 0x7ff80000 : (ULONG_PTR)0xfff80000;
#endif
}

///////////////////////////////////////////////////////////////////////// X86.
//
#ifdef DETOURS_X86

struct _DETOUR_TRAMPOLINE
{
    BYTE            rbCode[30];     // target code + jmp to pbRemain
    BYTE            cbCode;         // size of moved target code.
    BYTE            cbCodeBreak;    // padding to make debugging easier.
    BYTE            rbRestore[22];  // original target code.
    BYTE            cbRestore;      // size of original target code.
    BYTE            cbRestoreBreak; // padding to make debugging easier.
    _DETOUR_ALIGN   rAlign[8];      // instruction alignment array.
    PBYTE           pbRemain;       // first instruction after moved code. [free list]
    PBYTE           pbDetour;       // first instruction of detour function.
};

C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 72);

enum
{
    SIZE_OF_JMP = 5
};

inline PBYTE detour_gen_jmp_immediate(PBYTE pbCode, PBYTE pbJmpSrc, PBYTE pbJmpVal)
{
    pbJmpSrc = pbJmpSrc + 5;
    *pbCode++ = 0xE9;   // jmp +imm32
    *((INT32*&)pbCode)++ = (INT32)(pbJmpVal - pbJmpSrc);
    return pbCode;
}

inline PBYTE detour_gen_jmp_indirect(PBYTE pbCode, PBYTE *ppbJmpVal)
{
    *pbCode++ = 0xff;   // jmp [+imm32]
    *pbCode++ = 0x25;
    *((INT32*&)pbCode)++ = (INT32)((PBYTE)ppbJmpVal);
    return pbCode;
}

inline PBYTE detour_gen_brk(PBYTE pbCode, PBYTE pbLimit)
{
    while (pbCode < pbLimit)
    {
        *pbCode++ = 0xcc;   // brk;
    }
    return pbCode;
}

inline PBYTE detour_skip_jmp(PBYTE pbCode, PVOID *ppGlobals)
{
    if (pbCode == nullptr)
    {
        return nullptr;
    }
    if (ppGlobals != nullptr)
    {
        *ppGlobals = nullptr;
    }

    // First, skip over the import vector if there is one.
    if (pbCode[0] == 0xff && pbCode[1] == 0x25)
    {   
        // jmp [imm32]
        // Looks like an import alias jump, then get the code it points to.
        PBYTE pbTarget = *(UNALIGNED PBYTE *)&pbCode[2];
        if (detour_is_imported(pbCode, pbTarget))
        {
            PBYTE pbNew = *(UNALIGNED PBYTE *)pbTarget;
            DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
            pbCode = pbNew;
        }
    }

    // Then, skip over a patch jump
    if (pbCode[0] == 0xeb)
    {   
        // jmp +imm8
        PBYTE pbNew = pbCode + 2 + *(CHAR *)&pbCode[1];
        DETOUR_TRACE(("%p->%p: skipped over short jump.\n", pbCode, pbNew));
        pbCode = pbNew;

        // First, skip over the import vector if there is one.
        if (pbCode[0] == 0xff && pbCode[1] == 0x25)
        {   
            // jmp [imm32]
            // Looks like an import alias jump, then get the code it points to.
            PBYTE pbTarget = *(UNALIGNED PBYTE *)&pbCode[2];
            if (detour_is_imported(pbCode, pbTarget))
            {
                pbNew = *(UNALIGNED PBYTE *)pbTarget;
                DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
                pbCode = pbNew;
            }
        }
        // Finally, skip over a long jump if it is the target of the patch jump.
        else if (pbCode[0] == 0xe9)
        {   
            // jmp +imm32
            pbNew = pbCode + 5 + *(UNALIGNED INT32 *)&pbCode[1];
            DETOUR_TRACE(("%p->%p: skipped over long jump.\n", pbCode, pbNew));
            pbCode = pbNew;
        }
    }
    return pbCode;
}

inline void detour_find_jmp_bounds(PBYTE pbCode,
    PDETOUR_TRAMPOLINE *ppLower,
    PDETOUR_TRAMPOLINE *ppUpper)
{
    // We have to place trampolines within +/- 2GB of code.
    ULONG_PTR lo = detour_2gb_below((ULONG_PTR)pbCode);
    ULONG_PTR hi = detour_2gb_above((ULONG_PTR)pbCode);
    DETOUR_TRACE(("[%Ix..%p..%Ix]\n", lo, pbCode, hi));

    // And, within +/- 2GB of relative jmp targets.
    if (pbCode[0] == 0xe9)
    {   
        // jmp +imm32
        PBYTE pbNew = pbCode + 5 + *(UNALIGNED INT32 *)&pbCode[1];

        if (pbNew < pbCode)
        {
            hi = detour_2gb_above((ULONG_PTR)pbNew);
        }
        else
        {
            lo = detour_2gb_below((ULONG_PTR)pbNew);
        }
        DETOUR_TRACE(("[%Ix..%p..%Ix] +imm32\n", lo, pbCode, hi));
    }

    *ppLower = (PDETOUR_TRAMPOLINE)lo;
    *ppUpper = (PDETOUR_TRAMPOLINE)hi;
}

inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    if (pbCode[0] == 0xeb ||    // jmp +imm8
        pbCode[0] == 0xe9 ||    // jmp +imm32
        pbCode[0] == 0xe0 ||    // jmp eax
        pbCode[0] == 0xc2 ||    // ret +imm8
        pbCode[0] == 0xc3 ||    // ret
        pbCode[0] == 0xcc)
    {    
        // brk
        return TRUE;
    }
    else if (pbCode[0] == 0xf3 && pbCode[1] == 0xc3)
    {  
        // rep ret
        return TRUE;
    }
    else if (pbCode[0] == 0xff && pbCode[1] == 0x25)
    {  
        // jmp [+imm32]
        return TRUE;
    }
    else if ((
        pbCode[0] == 0x26 ||    // jmp es:
        pbCode[0] == 0x2e ||    // jmp cs:
        pbCode[0] == 0x36 ||    // jmp ss:
        pbCode[0] == 0x3e ||    // jmp ds:
        pbCode[0] == 0x64 ||    // jmp fs:
        pbCode[0] == 0x65       // jmp gs:
        ) &&                      
        pbCode[1] == 0xff &&    // jmp [+imm32]
        pbCode[2] == 0x25)
    {
        return TRUE;
    }
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    // 1-byte through 11-byte NOPs.
    if (pbCode[0] == 0x90)
    {
        return 1;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x90)
    {
        return 2;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x00)
    {
        return 3;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x40 &&
        pbCode[3] == 0x00)
    {
        return 4;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x44 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00)
    {
        return 5;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x0F && pbCode[2] == 0x1F &&
        pbCode[3] == 0x44 && pbCode[4] == 0x00 && pbCode[5] == 0x00)
    {
        return 6;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x80 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00)
    {
        return 7;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x84 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00)
    {
        return 8;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x0F && pbCode[2] == 0x1F &&
        pbCode[3] == 0x84 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00)
    {
        return 9;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x66 && pbCode[2] == 0x0F &&
        pbCode[3] == 0x1F && pbCode[4] == 0x84 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00 &&
        pbCode[9] == 0x00)
    {
        return 10;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x66 && pbCode[2] == 0x66 &&
        pbCode[3] == 0x0F && pbCode[4] == 0x1F && pbCode[5] == 0x84 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00 &&
        pbCode[9] == 0x00 && pbCode[10] == 0x00)
    {
        return 11;
    }

    // int 3.
    if (pbCode[0] == 0xcc)
    {
        return 1;
    }
    return 0;
}

#endif // DETOURS_X86

///////////////////////////////////////////////////////////////////////// X64.
//
#ifdef DETOURS_X64

struct _DETOUR_TRAMPOLINE
{
    // An X64 instuction can be 15 bytes long.
    // In practice 11 seems to be the limit.
    BYTE            rbCode[30];     // target code + jmp to pbRemain.
    BYTE            cbCode;         // size of moved target code.
    BYTE            cbCodeBreak;    // padding to make debugging easier.
    BYTE            rbRestore[30];  // original target code.
    BYTE            cbRestore;      // size of original target code.
    BYTE            cbRestoreBreak; // padding to make debugging easier.
    _DETOUR_ALIGN   rAlign[8];      // instruction alignment array.
    PBYTE           pbRemain;       // first instruction after moved code. [free list]
    PBYTE           pbDetour;       // first instruction of detour function.
    BYTE            rbCodeIn[8];    // jmp [pbDetour]
};

C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 96);

enum
{
    SIZE_OF_JMP             = 12,
    SIZE_OF_JMP_TO_REMAIN   = 6
};

inline PBYTE detour_gen_jmp_far(PBYTE pbCode, PBYTE pbJmpVal)
{
    //const BYTE Jumper[] = 
    //{ 
    //    // 50                             push   rax
    //    // 48 b8 00 00 00 00 00 00 00 00  mov rax, 0x0
    //    // 48 87 04 24                    xchg   QWORD PTR[rsp], rax
    //    // c3                             ret
    //
    //    0x50,   //  vvvvvvvvvvvvvvvvv address vvvvvvvvvvvvvvvvvvvv
    //    0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    //    0x48, 0x87, 0x04, 0x24,
    //    0xc3
    //};
    //C_ASSERT(sizeof(Jumper) == SIZE_OF_JMP);

    const BYTE Jumper[] =
    {
        // 48 b8 00 00 00 00 00 00 00 00  mov rax, 0x0
        // 50                             push   rax
        // c3                             ret

        //          vvvvvvvvvvvvvvvvv address vvvvvvvvvvvvvvvvvvvv
        0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x50,
        0xc3
    };
    C_ASSERT(sizeof(Jumper) == SIZE_OF_JMP);
    
    memcpy(pbCode, Jumper, sizeof(Jumper));
    memcpy(pbCode + 2,  &pbJmpVal, sizeof(&pbJmpVal));

    pbCode += sizeof(Jumper);
    return pbCode;
}

inline PBYTE detour_gen_jmp_immediate(PBYTE pbCode, PBYTE pbJmpSrc, PBYTE pbJmpVal)
{
    pbJmpSrc = pbJmpSrc + 5;
    *pbCode++ = 0xE9;   // jmp +imm32
    *((INT32*&)pbCode)++ = (INT32)(pbJmpVal - pbJmpSrc);
    return pbCode;
}

inline PBYTE detour_gen_jmp_indirect(PBYTE pbCode, PBYTE pbJmpSrc, PBYTE *ppbJmpVal)
{
    pbJmpSrc = pbJmpSrc + 6;
    *pbCode++ = 0xff;   // jmp [+imm32]
    *pbCode++ = 0x25;
    *((INT32*&)pbCode)++ = (INT32)((PBYTE)ppbJmpVal - pbJmpSrc);
    return pbCode;
}

inline PBYTE detour_gen_brk(PBYTE pbCode, PBYTE pbLimit)
{
    while (pbCode < pbLimit)
    {
        *pbCode++ = 0xcc;   // brk;
    }
    return pbCode;
}

inline PBYTE detour_skip_jmp(PBYTE pbCode, PVOID *ppGlobals)
{
    if (pbCode == nullptr)
    {
        return nullptr;
    }
    if (ppGlobals != nullptr)
    {
        *ppGlobals = nullptr;
    }

    // First, skip over the import vector if there is one.
    if (pbCode[0] == 0xff && pbCode[1] == 0x25)
    {   
        // jmp [+imm32]
        // Looks like an import alias jump, then get the code it points to.
        PBYTE pbTarget = pbCode + 6 + *(UNALIGNED INT32 *)&pbCode[2];
        if (detour_is_imported(pbCode, pbTarget))
        {
            PBYTE pbNew = *(UNALIGNED PBYTE *)pbTarget;
            DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
            pbCode = pbNew;
        }
    }

    // Then, skip over a patch jump
    if (pbCode[0] == 0xeb)
    {   
        // jmp +imm8
        PBYTE pbNew = pbCode + 2 + *(CHAR *)&pbCode[1];
        DETOUR_TRACE(("%p->%p: skipped over short jump.\n", pbCode, pbNew));
        pbCode = pbNew;

        // First, skip over the import vector if there is one.
        if (pbCode[0] == 0xff && pbCode[1] == 0x25)
        {   
            // jmp [+imm32]
            // Looks like an import alias jump, then get the code it points to.
            PBYTE pbTarget = pbCode + 6 + *(UNALIGNED INT32 *)&pbCode[2];
            if (detour_is_imported(pbCode, pbTarget))
            {
                pbNew = *(UNALIGNED PBYTE *)pbTarget;
                DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
                pbCode = pbNew;
            }
        }
        // Finally, skip over a long jump if it is the target of the patch jump.
        else if (pbCode[0] == 0xe9)
        {   
            // jmp +imm32
            pbNew = pbCode + 5 + *(UNALIGNED INT32 *)&pbCode[1];
            DETOUR_TRACE(("%p->%p: skipped over long jump.\n", pbCode, pbNew));
            pbCode = pbNew;
        }
    }
    return pbCode;
}

inline void detour_find_jmp_bounds(PBYTE pbCode,
    PDETOUR_TRAMPOLINE *ppLower,
    PDETOUR_TRAMPOLINE *ppUpper)
{
    // We have to place trampolines within +/- 2GB of code.
    ULONG_PTR lo = detour_2gb_below((ULONG_PTR)pbCode);
    ULONG_PTR hi = detour_2gb_above((ULONG_PTR)pbCode);
    DETOUR_TRACE(("[%Ix..%p..%Ix]\n", lo, pbCode, hi));

    // And, within +/- 2GB of relative jmp vectors.
    if (pbCode[0] == 0xff && pbCode[1] == 0x25)
    {   
        // jmp [+imm32]
        PBYTE pbNew = pbCode + 6 + *(UNALIGNED INT32 *)&pbCode[2];

        if (pbNew < pbCode)
        {
            hi = detour_2gb_above((ULONG_PTR)pbNew);
        }
        else
        {
            lo = detour_2gb_below((ULONG_PTR)pbNew);
        }
        DETOUR_TRACE(("[%Ix..%p..%Ix] [+imm32]\n", lo, pbCode, hi));
    }
    // And, within +/- 2GB of relative jmp targets.
    else if (pbCode[0] == 0xe9)
    {   
        // jmp +imm32
        PBYTE pbNew = pbCode + 5 + *(UNALIGNED INT32 *)&pbCode[1];

        if (pbNew < pbCode)
        {
            hi = detour_2gb_above((ULONG_PTR)pbNew);
        }
        else
        {
            lo = detour_2gb_below((ULONG_PTR)pbNew);
        }
        DETOUR_TRACE(("[%Ix..%p..%Ix] +imm32\n", lo, pbCode, hi));
    }

    *ppLower = (PDETOUR_TRAMPOLINE)lo;
    *ppUpper = (PDETOUR_TRAMPOLINE)hi;
}

inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    if (pbCode[0] == 0xeb ||    // jmp +imm8
        pbCode[0] == 0xe9 ||    // jmp +imm32
        pbCode[0] == 0xe0 ||    // jmp eax
        pbCode[0] == 0xc2 ||    // ret +imm8
        pbCode[0] == 0xc3 ||    // ret
        pbCode[0] == 0xcc)
    {    
        // brk
        return TRUE;
    }
    else if (pbCode[0] == 0xf3 && pbCode[1] == 0xc3)
    {  
        // rep ret
        return TRUE;
    }
    else if (pbCode[0] == 0xff && pbCode[1] == 0x25)
    {  
        // jmp [+imm32]
        return TRUE;
    }
    else if ((
        pbCode[0] == 0x26 ||    // jmp es:
        pbCode[0] == 0x2e ||    // jmp cs:
        pbCode[0] == 0x36 ||    // jmp ss:
        pbCode[0] == 0x3e ||    // jmp ds:
        pbCode[0] == 0x64 ||    // jmp fs:
        pbCode[0] == 0x65       // jmp gs:
        ) &&     
        pbCode[1] == 0xff &&    // jmp [+imm32]
        pbCode[2] == 0x25)
    {
        return TRUE;
    }
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    // 1-byte through 11-byte NOPs.
    if (pbCode[0] == 0x90)
    {
        return 1;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x90)
    {
        return 2;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x00)
    {
        return 3;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x40 &&
        pbCode[3] == 0x00)
    {
        return 4;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x44 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00)
    {
        return 5;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x0F && pbCode[2] == 0x1F &&
        pbCode[3] == 0x44 && pbCode[4] == 0x00 && pbCode[5] == 0x00)
    {
        return 6;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x80 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00)
    {
        return 7;
    }
    if (pbCode[0] == 0x0F && pbCode[1] == 0x1F && pbCode[2] == 0x84 &&
        pbCode[3] == 0x00 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00)
    {
        return 8;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x0F && pbCode[2] == 0x1F &&
        pbCode[3] == 0x84 && pbCode[4] == 0x00 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00)
    {
        return 9;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x66 && pbCode[2] == 0x0F &&
        pbCode[3] == 0x1F && pbCode[4] == 0x84 && pbCode[5] == 0x00 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00 &&
        pbCode[9] == 0x00)
    {
        return 10;
    }
    if (pbCode[0] == 0x66 && pbCode[1] == 0x66 && pbCode[2] == 0x66 &&
        pbCode[3] == 0x0F && pbCode[4] == 0x1F && pbCode[5] == 0x84 &&
        pbCode[6] == 0x00 && pbCode[7] == 0x00 && pbCode[8] == 0x00 &&
        pbCode[9] == 0x00 && pbCode[10] == 0x00)
    {
        return 11;
    }

    // int 3.
    if (pbCode[0] == 0xcc)
    {
        return 1;
    }
    return 0;
}

#endif // DETOURS_X64

//////////////////////////////////////////////////////////////////////// IA64.
//
#ifdef DETOURS_IA64

struct _DETOUR_TRAMPOLINE
{
    // On the IA64, a trampoline is used for both incoming and outgoing calls.
    //
    // The trampoline contains the following bundles for the outgoing call:
    //      movl gp=target_gp;
    //      <relocated target bundle>
    //      brl  target_code;
    //
    // The trampoline contains the following bundles for the incoming call:
    //      alloc  r41=ar.pfs, b, 0, 8, 0
    //      mov    r40=rp
    //
    //      adds   r50=0, r39
    //      adds   r49=0, r38
    //      adds   r48=0, r37 ;;
    //
    //      adds   r47=0, r36
    //      adds   r46=0, r35
    //      adds   r45=0, r34
    //
    //      adds   r44=0, r33
    //      adds   r43=0, r32
    //      adds   r42=0, gp ;;
    //
    //      movl   gp=ffffffff`ffffffff ;;
    //
    //      brl.call.sptk.few rp=disas!TestCodes+20e0 (00000000`00404ea0) ;;
    //
    //      adds   gp=0, r42
    //      mov    rp=r40, +0 ;;
    //      mov.i  ar.pfs=r41
    //
    //      br.ret.sptk.many rp ;;
    //
    // This way, we only have to relocate a single bundle.
    //
    // The complicated incoming trampoline is required because we have to
    // create an additional stack frame so that we save and restore the gp.
    // We must do this because gp is a caller-saved register, but not saved
    // if the caller thinks the target is in the same DLL, which changes
    // when we insert a detour.
    //
    DETOUR_IA64_BUNDLE  bMovlTargetGp;  // Bundle which sets target GP
    BYTE                rbCode[sizeof(DETOUR_IA64_BUNDLE)]; // moved bundle.
    DETOUR_IA64_BUNDLE  bBrlRemainEip;  // Brl to pbRemain
    // This must be adjacent to bBranchIslands.

    // Each instruction in the moved bundle could be a IP-relative chk or branch or call.
    // Any such instructions are changed to point to a brl in bBranchIslands.
    // This must be adjacent to bBrlRemainEip -- see "pbPool".
    DETOUR_IA64_BUNDLE bBranchIslands[DETOUR_IA64_INSTRUCTIONS_PER_BUNDLE];

    // Target of brl inserted in target function
    DETOUR_IA64_BUNDLE  bAllocFrame;    // alloc frame
    DETOUR_IA64_BUNDLE  bSave37to39;    // save r37, r38, r39.
    DETOUR_IA64_BUNDLE  bSave34to36;    // save r34, r35, r36.
    DETOUR_IA64_BUNDLE  bSaveGPto33;    // save gp, r32, r33.
    DETOUR_IA64_BUNDLE  bMovlDetourGp;  // set detour GP.
    DETOUR_IA64_BUNDLE  bCallDetour;    // call detour.
    DETOUR_IA64_BUNDLE  bPopFrameGp;    // pop frame and restore gp.
    DETOUR_IA64_BUNDLE  bReturn;        // return to caller.

    PLABEL_DESCRIPTOR   pldTrampoline;

    BYTE                rbRestore[sizeof(DETOUR_IA64_BUNDLE)]; // original target bundle.
    BYTE                cbRestore;      // size of original target code.
    BYTE                cbCode;         // size of moved target code.
    _DETOUR_ALIGN       rAlign[14];     // instruction alignment array.
    PBYTE               pbRemain;       // first instruction after moved code. [free list]
    PBYTE               pbDetour;       // first instruction of detour function.
    PPLABEL_DESCRIPTOR  ppldDetour;     // [pbDetour,gpDetour]
    PPLABEL_DESCRIPTOR  ppldTarget;     // [pbTarget,gpDetour]
};

C_ASSERT(sizeof(DETOUR_IA64_BUNDLE) == 16);
C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 256 + DETOUR_IA64_INSTRUCTIONS_PER_BUNDLE * 16);

enum
{
    SIZE_OF_JMP = sizeof(DETOUR_IA64_BUNDLE)
};

inline PBYTE detour_skip_jmp(PBYTE pPointer, PVOID *ppGlobals)
{
    PBYTE pGlobals = nullptr;
    PBYTE pbCode = nullptr;

    if (pPointer != nullptr)
    {
        PPLABEL_DESCRIPTOR ppld = (PPLABEL_DESCRIPTOR)pPointer;
        pbCode = (PBYTE)ppld->EntryPoint;
        pGlobals = (PBYTE)ppld->GlobalPointer;
    }
    if (ppGlobals != nullptr)
    {
        *ppGlobals = pGlobals;
    }
    if (pbCode == nullptr)
    {
        return nullptr;
    }

    DETOUR_IA64_BUNDLE *pb = (DETOUR_IA64_BUNDLE *)pbCode;

    // IA64 Local Import Jumps look like:
    //      addl   r2=ffffffff`ffe021c0, gp ;;
    //      ld8    r2=[r2]
    //      nop.i  0 ;;
    //
    //      ld8    r3=[r2], 8 ;;
    //      ld8    gp=[r2]
    //      mov    b6=r3, +0
    //
    //      nop.m  0
    //      nop.i  0
    //      br.cond.sptk.few b6
    //

    //                     002024000200100b
    if ((pb[0].wide[0] & 0xfffffc000603ffff) == 0x002024000200100b &&
        pb[0].wide[1] == 0x0004000000203008 &&
        pb[1].wide[0] == 0x001014180420180a &&
        pb[1].wide[1] == 0x07000830c0203008 &&
        pb[2].wide[0] == 0x0000000100000010 &&
        pb[2].wide[1] == 0x0080006000000200)
    {

        ULONG64 offset =
            ((pb[0].wide[0] & 0x0000000001fc0000) >> 18) |  // imm7b
            ((pb[0].wide[0] & 0x000001ff00000000) >> 25) |  // imm9d
            ((pb[0].wide[0] & 0x00000000f8000000) >> 11);   // imm5c
        if (pb[0].wide[0] & 0x0000020000000000)
        {           
            //          sign
            offset |= 0xffffffffffe00000;
        }
        PBYTE pbTarget = pGlobals + offset;
        DETOUR_TRACE(("%p: potential import jump, target=%p\n", pb, pbTarget));

        if (detour_is_imported(pbCode, pbTarget) && *(PBYTE*)pbTarget != nullptr)
        {
            DETOUR_TRACE(("%p: is import jump, label=%p\n", pb, *(PBYTE *)pbTarget));

            PPLABEL_DESCRIPTOR ppld = (PPLABEL_DESCRIPTOR)*(PBYTE *)pbTarget;
            pbCode = (PBYTE)ppld->EntryPoint;
            pGlobals = (PBYTE)ppld->GlobalPointer;
            if (ppGlobals != nullptr)
            {
                *ppGlobals = pGlobals;
            }
        }
    }
    return pbCode;
}


inline void detour_find_jmp_bounds(PBYTE pbCode,
    PDETOUR_TRAMPOLINE *ppLower,
    PDETOUR_TRAMPOLINE *ppUpper)
{
    (void)pbCode;
    *ppLower = (PDETOUR_TRAMPOLINE)(ULONG_PTR)0x0000000000080000;
    *ppUpper = (PDETOUR_TRAMPOLINE)(ULONG_PTR)0xfffffffffff80000;
}

inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    // Routine not needed on IA64.
    (void)pbCode;
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    // Routine not needed on IA64.
    (void)pbCode;
    return 0;
}

#endif // DETOURS_IA64

#ifdef DETOURS_ARM

struct _DETOUR_TRAMPOLINE
{
    // A Thumb-2 instruction can be 2 or 4 bytes long.
    BYTE            rbCode[62];     // target code + jmp to pbRemain
    BYTE            cbCode;         // size of moved target code.
    BYTE            cbCodeBreak;    // padding to make debugging easier.
    BYTE            rbRestore[22];  // original target code.
    BYTE            cbRestore;      // size of original target code.
    BYTE            cbRestoreBreak; // padding to make debugging easier.
    _DETOUR_ALIGN   rAlign[8];      // instruction alignment array.
    PBYTE           pbRemain;       // first instruction after moved code. [free list]
    PBYTE           pbDetour;       // first instruction of detour function.
};

C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 104);

enum
{
    SIZE_OF_JMP = 8
};

inline PBYTE align4(PBYTE pValue)
{
    return (PBYTE)(((ULONG)pValue) & ~(ULONG)3u);
}

inline ULONG fetch_thumb_opcode(PBYTE pbCode)
{
    ULONG Opcode = *(UINT16 *)&pbCode[0];
    if (Opcode >= 0xe800)
    {
        Opcode = (Opcode << 16) | *(UINT16 *)&pbCode[2];
    }
    return Opcode;
}

inline void write_thumb_opcode(PBYTE &pbCode, ULONG Opcode)
{
    if (Opcode >= 0x10000)
    {
        *((UINT16*&)pbCode)++ = Opcode >> 16;
    }
    *((UINT16*&)pbCode)++ = (UINT16)Opcode;
}

PBYTE detour_gen_jmp_immediate(PBYTE pbCode, PBYTE *ppPool, PBYTE pbJmpVal)
{
    PBYTE pbLiteral;
    if (ppPool != nullptr)
    {
        *ppPool = *ppPool - 4;
        pbLiteral = *ppPool;
    }
    else
    {
        pbLiteral = align4(pbCode + 6);
    }

    *((PBYTE*&)pbLiteral) = DETOURS_PBYTE_TO_PFUNC(pbJmpVal);
    LONG delta = pbLiteral - align4(pbCode + 4);

    write_thumb_opcode(pbCode, 0xf8dff000 | delta);     // LDR PC,[PC+n]

    if (ppPool == nullptr)
    {
        if (((ULONG)pbCode & 2) != 0)
        {
            write_thumb_opcode(pbCode, 0xdefe);         // BREAK
        }
        pbCode += 4;
    }
    return pbCode;
}

inline PBYTE detour_gen_brk(PBYTE pbCode, PBYTE pbLimit)
{
    while (pbCode < pbLimit)
    {
        write_thumb_opcode(pbCode, 0xdefe);
    }
    return pbCode;
}

inline PBYTE detour_skip_jmp(PBYTE pbCode, PVOID *ppGlobals)
{
    if (pbCode == nullptr)
    {
        return nullptr;
    }
    if (ppGlobals != nullptr)
    {
        *ppGlobals = nullptr;
    }

    // Skip over the import jump if there is one.
    pbCode = (PBYTE)DETOURS_PFUNC_TO_PBYTE(pbCode);
    ULONG Opcode = fetch_thumb_opcode(pbCode);

    if ((Opcode & 0xfbf08f00) == 0xf2400c00)
    {          
        // movw r12,#xxxx
        ULONG Opcode2 = fetch_thumb_opcode(pbCode + 4);

        if ((Opcode2 & 0xfbf08f00) == 0xf2c00c00)
        {      
            // movt r12,#xxxx
            ULONG Opcode3 = fetch_thumb_opcode(pbCode + 8);
            if (Opcode3 == 0xf8dcf000)
            {                 
                // ldr  pc,[r12]
                PBYTE pbTarget = (PBYTE)(((Opcode2 << 12) & 0xf7000000) |
                    ((Opcode2 << 1) & 0x08000000) |
                    ((Opcode2 << 16) & 0x00ff0000) |
                    ((Opcode >> 4) & 0x0000f700) |
                    ((Opcode >> 15) & 0x00000800) |
                    ((Opcode >> 0) & 0x000000ff));
                if (detour_is_imported(pbCode, pbTarget))
                {
                    PBYTE pbNew = *(PBYTE *)pbTarget;
                    pbNew = DETOURS_PFUNC_TO_PBYTE(pbNew);
                    DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
                    return pbNew;
                }
            }
        }
    }
    return pbCode;
}

inline void detour_find_jmp_bounds(PBYTE pbCode,
    PDETOUR_TRAMPOLINE *ppLower,
    PDETOUR_TRAMPOLINE *ppUpper)
{
    // We have to place trampolines within +/- 2GB of code.
    ULONG_PTR lo = detour_2gb_below((ULONG_PTR)pbCode);
    ULONG_PTR hi = detour_2gb_above((ULONG_PTR)pbCode);
    DETOUR_TRACE(("[%p..%p..%p]\n", lo, pbCode, hi));

    *ppLower = (PDETOUR_TRAMPOLINE)lo;
    *ppUpper = (PDETOUR_TRAMPOLINE)hi;
}


inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    ULONG Opcode = fetch_thumb_opcode(pbCode);
    if ((Opcode & 0xffffff87) == 0x4700 ||          // bx <reg>
        (Opcode & 0xf800d000) == 0xf0009000)
    {      
        // b <imm20>
        return TRUE;
    }
    if ((Opcode & 0xffff8000) == 0xe8bd8000)
    {      
        // pop {...,pc}
        DETOUR_BREAK();
        return TRUE;
    }
    if ((Opcode & 0xffffff00) == 0x0000bd00)
    {      
        // pop {...,pc}
        DETOUR_BREAK();
        return TRUE;
    }
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    if (pbCode[0] == 0x00 && pbCode[1] == 0xbf)
    { 
        // nop.
        return 2;
    }
    if (pbCode[0] == 0x00 && pbCode[1] == 0x00)
    { 
        // zero-filled padding.
        return 2;
    }
    return 0;
}

#endif // DETOURS_ARM

#ifdef DETOURS_ARM64

struct _DETOUR_TRAMPOLINE
{
    // An ARM64 instruction is 4 bytes long.
    //
    // The overwrite is always 2 instructions plus a literal, so 16 bytes, 4 instructions.
    //
    // Copied instructions can expand.
    //
    // The scheme using MovImmediate can cause an instruction
    // to grow as much as 6 times.
    // That would be Bcc or Tbz with a large address space:
    //   4 instructions to form immediate
    //   inverted tbz/bcc
    //   br
    //
    // An expansion of 4 is not uncommon -- bl/blr and small address space:
    //   3 instructions to form immediate
    //   br or brl
    //
    // A theoretical maximum for rbCode is thefore 4*4*6 + 16 = 112 (another 16 for jmp to pbRemain).
    //
    // With literals, the maximum expansion is 5, including the literals: 4*4*5 + 16 = 96.
    //
    // The number is rounded up to 128. m_rbScratchDst should match this.
    //
    BYTE            rbCode[128];    // target code + jmp to pbRemain
    BYTE            cbCode;         // size of moved target code.
    BYTE            cbCodeBreak[3]; // padding to make debugging easier.
    BYTE            rbRestore[24];  // original target code.
    BYTE            cbRestore;      // size of original target code.
    BYTE            cbRestoreBreak[3]; // padding to make debugging easier.
    _DETOUR_ALIGN   rAlign[8];      // instruction alignment array.
    PBYTE           pbRemain;       // first instruction after moved code. [free list]
    PBYTE           pbDetour;       // first instruction of detour function.
};

C_ASSERT(sizeof(_DETOUR_TRAMPOLINE) == 184);

enum
{
    SIZE_OF_JMP = 16
};

inline ULONG fetch_opcode(PBYTE pbCode)
{
    return *(ULONG *)pbCode;
}

inline void write_opcode(PBYTE &pbCode, ULONG Opcode)
{
    *(ULONG *)pbCode = Opcode;
    pbCode += 4;
}

PBYTE detour_gen_jmp_immediate(PBYTE pbCode, PBYTE *ppPool, PBYTE pbJmpVal)
{
    PBYTE pbLiteral;
    if (ppPool != nullptr)
    {
        *ppPool = *ppPool - 8;
        pbLiteral = *ppPool;
    }
    else
    {
        pbLiteral = pbCode + 8;
    }

    *((PBYTE*&)pbLiteral) = pbJmpVal;
    LONG delta = (LONG)(pbLiteral - pbCode);

    write_opcode(pbCode, 0x58000011 | ((delta / 4) << 5));  // LDR X17,[PC+n]
    write_opcode(pbCode, 0xd61f0000 | (17 << 5));           // BR X17

    if (ppPool == nullptr)
    {
        pbCode += 8;
    }
    return pbCode;
}

inline PBYTE detour_gen_brk(PBYTE pbCode, PBYTE pbLimit)
{
    while (pbCode < pbLimit)
    {
        write_opcode(pbCode, 0xd4100000 | (0xf000 << 5));
    }
    return pbCode;
}

inline INT64 detour_sign_extend(UINT64 value, UINT bits)
{
    const UINT left = 64 - bits;
    const INT64 m1 = -1;
    const INT64 wide = (INT64)(value << left);
    const INT64 sign = (wide < 0) ? (m1 << left) : 0;
    return value | sign;
}

inline PBYTE detour_skip_jmp(PBYTE pbCode, PVOID *ppGlobals)
{
    if (pbCode == nullptr)
    {
        return nullptr;
    }
    if (ppGlobals != nullptr)
    {
        *ppGlobals = nullptr;
    }

    // Skip over the import jump if there is one.
    pbCode = (PBYTE)pbCode;
    ULONG Opcode = fetch_opcode(pbCode);

    if ((Opcode & 0x9f00001f) == 0x90000010)
    {           
        // adrp  x16, IAT
        ULONG Opcode2 = fetch_opcode(pbCode + 4);

        if ((Opcode2 & 0xffe003ff) == 0xf9400210)
        {      
            // ldr   x16, [x16, IAT]
            ULONG Opcode3 = fetch_opcode(pbCode + 8);

            if (Opcode3 == 0xd61f0200)
            {                 
                // br    x16

                /* https://static.docs.arm.com/ddi0487/bb/DDI0487B_b_armv8_arm.pdf
                    The ADRP instruction shifts a signed, 21-bit immediate left by 12 bits, adds it to the value of the program counter with
                    the bottom 12 bits cleared to zero, and then writes the result to a general-purpose register. This permits the
                    calculation of the address at a 4KB aligned memory region. In conjunction with an ADD (immediate) instruction, or
                    a Load/Store instruction with a 12-bit immediate offset, this allows for the calculation of, or access to, any address
                    within ?GB of the current PC.

                PC-rel. addressing
                    This section describes the encoding of the PC-rel. addressing instruction class. The encodings in this section are
                    decoded from Data Processing -- Immediate on page C4-226.
                    Add/subtract (immediate)
                    This section describes the encoding of the Add/subtract (immediate) instruction class. The encodings in this section
                    are decoded from Data Processing -- Immediate on page C4-226.
                    Decode fields
                    Instruction page
                    op
                    0 ADR
                    1 ADRP

                C6.2.10 ADRP
                    Form PC-relative address to 4KB page adds an immediate value that is shifted left by 12 bits, to the PC value to
                    form a PC-relative address, with the bottom 12 bits masked out, and writes the result to the destination register.
                    ADRP <Xd>, <label>
                    imm = SignExtend(immhi:immlo:Zeros(12), 64);

                    31  30 29 28 27 26 25 24 23 5    4 0
                    1   immlo  1  0  0  0  0  immhi  Rd
                         9             0

                Rd is hardcoded as 0x10 above.
                Immediate is 21 signed bits split into 2 bits and 19 bits, and is scaled by 4K.
                */
                UINT64 const pageLow2 = (Opcode >> 29) & 3;
                UINT64 const pageHigh19 = (Opcode >> 5) & ~(~0ui64 << 19);
                INT64 const page = detour_sign_extend((pageHigh19 << 2) | pageLow2, 21) << 12;

                /* https://static.docs.arm.com/ddi0487/bb/DDI0487B_b_armv8_arm.pdf

                    C6.2.101 LDR (immediate)
                    Load Register (immediate) loads a word or doubleword from memory and writes it to a register. The address that is
                    used for the load is calculated from a base register and an immediate offset.
                    The Unsigned offset variant scales the immediate offset value by the size of the value accessed before adding it
                    to the base register value.

                Unsigned offset
                64-bit variant Applies when size == 11.
                    31 30 29 28  27 26 25 24  23 22  21   10   9 5   4 0
                     1  x  1  1   1  0  0  1   0  1  imm12      Rn    Rt
                         F             9        4              200    10

                That is, two low 5 bit fields are registers, hardcoded as 0x10 and 0x10 << 5 above,
                then unsigned size-unscaled (8) 12-bit offset, then opcode bits 0xF94.
                */
                UINT64 const offset = ((Opcode2 >> 10) & ~(~0ui64 << 12)) << 3;

                PBYTE const pbTarget = (PBYTE)((ULONG64)pbCode & 0xfffffffffffff000ULL) + page + offset;

                if (detour_is_imported(pbCode, pbTarget))
                {
                    PBYTE pbNew = *(PBYTE *)pbTarget;
                    DETOUR_TRACE(("%p->%p: skipped over import table.\n", pbCode, pbNew));
                    return pbNew;
                }
            }
        }
    }
    return pbCode;
}

inline void detour_find_jmp_bounds(PBYTE pbCode,
    PDETOUR_TRAMPOLINE *ppLower,
    PDETOUR_TRAMPOLINE *ppUpper)
{
    // We have to place trampolines within +/- 2GB of code.
    ULONG_PTR lo = detour_2gb_below((ULONG_PTR)pbCode);
    ULONG_PTR hi = detour_2gb_above((ULONG_PTR)pbCode);
    DETOUR_TRACE(("[%p..%p..%p]\n", lo, pbCode, hi));

    *ppLower = (PDETOUR_TRAMPOLINE)lo;
    *ppUpper = (PDETOUR_TRAMPOLINE)hi;
}

inline BOOL detour_does_code_end_function(PBYTE pbCode)
{
    ULONG Opcode = fetch_opcode(pbCode);
    if ((Opcode & 0xfffffc1f) == 0xd65f0000 ||      // br <reg>
        (Opcode & 0xfc000000) == 0x14000000)
    {      
        // b <imm26>
        return TRUE;
    }
    return FALSE;
}

inline ULONG detour_is_code_filler(PBYTE pbCode)
{
    if (*(ULONG *)pbCode == 0xd503201f)
    {   
        // nop.
        return 4;
    }
    if (*(ULONG *)pbCode == 0x00000000)
    {   
        // zero-filled padding.
        return 4;
    }
    return 0;
}

#endif // DETOURS_ARM64

//////////////////////////////////////////////// Trampoline Memory Management.
//
typedef struct DETOUR_REGION
{
    ULONG               dwSignature;
    PMDL                pMdl;   // Trampoline regions's mdl
    DETOUR_REGION *     pNext;  // Next region in list of regions.
    DETOUR_TRAMPOLINE * pFree;  // List of free trampolines in this region.
}*PDETOUR_REGION;

C_ASSERT(sizeof(DETOUR_REGION) <= sizeof(DETOUR_TRAMPOLINE));

const ULONG DETOUR_REGION_SIGNATURE = 'Rrtd';
const ULONG DETOUR_REGION_SIZE      = PAGE_SIZE;
const ULONG DETOUR_TRAMPOLINES_PER_REGION = (DETOUR_REGION_SIZE / sizeof(DETOUR_TRAMPOLINE)) - 1;
static PDETOUR_REGION s_pRegions    = nullptr;      // List of all regions.
static PDETOUR_REGION s_pRegion     = nullptr;      // Default region.

static DWORD detour_writable_trampoline_regions()
{
    DWORD Status = DETOURS_STATUS_SUCCESS;

    // Mark all of the regions as writable.
    for (PDETOUR_REGION pRegion = s_pRegions; pRegion != nullptr; pRegion = pRegion->pNext)
    {
        Status = MmProtectMdlSystemAddress(pRegion->pMdl, PAGE_EXECUTE_READWRITE);
        if (!NT_SUCCESS(Status))
        {
            return DetoursSetLastError(Status), Status;
        }
    }

    return Status;
}

static void detour_runnable_trampoline_regions()
{
    // Mark all of the regions as executable.
    for (PDETOUR_REGION pRegion = s_pRegions; pRegion != nullptr; pRegion = pRegion->pNext)
    {
        MmProtectMdlSystemAddress(pRegion->pMdl, PAGE_EXECUTE_READ);
    }
}

static PDETOUR_TRAMPOLINE detour_alloc_trampoline(PBYTE pbTarget)
{
    // We have to place trampolines within +/- 2GB of target.

    PDETOUR_TRAMPOLINE pLo;
    PDETOUR_TRAMPOLINE pHi;

    detour_find_jmp_bounds(pbTarget, &pLo, &pHi);

    PDETOUR_TRAMPOLINE pTrampoline = nullptr;

    // Insure that there is a default region.
    if (s_pRegion == nullptr && s_pRegions != nullptr)
    {
        s_pRegion = s_pRegions;
    }

    // First check the default region for an valid free block.
    if (s_pRegion != nullptr && s_pRegion->pFree != nullptr)
    {

    found_region:
        pTrampoline = s_pRegion->pFree;

#ifndef DETOURS_X64
        // do a last sanity check on region.
        if (pTrampoline < pLo || pTrampoline > pHi)
        {
            return nullptr;
        }
#endif
        s_pRegion->pFree = (PDETOUR_TRAMPOLINE)pTrampoline->pbRemain;
        memset(pTrampoline, 0xcc, sizeof(*pTrampoline));
        return pTrampoline;
    }

    // Then check the existing regions for a valid free block.
    for (s_pRegion = s_pRegions; s_pRegion != nullptr; s_pRegion = s_pRegion->pNext)
    {
        if (s_pRegion != nullptr && s_pRegion->pFree != nullptr)
        {
            goto found_region;
        }
    }

    // We need to allocate a new region.

    // Round pbTarget down to 64KB block.
    pbTarget = pbTarget - (PtrToUlong(pbTarget) & 0xffff);

    PVOID pbTry = nullptr;
    PMDL  pMdl  = nullptr;

    // NB: We must always also start the search at an offset from pbTarget
    //     in order to maintain ASLR entropy.

    PHYSICAL_ADDRESS EmptyDesc = { 0 };
    PHYSICAL_ADDRESS MaxAddress;
    MaxAddress.QuadPart = _UI64_MAX;

    pMdl = MmAllocatePagesForMdlEx(
        EmptyDesc, MaxAddress, EmptyDesc, DETOUR_REGION_SIZE,
        MmNonCached, MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS);
    if (pMdl != nullptr)
    {
        pbTry = MmGetSystemAddressForMdlSafe(pMdl, HighPagePriority);
        if (pbTry == nullptr)
        {
            MmFreePagesFromMdl(pMdl);
            ExFreePool(pMdl);
        }
    }

    if (pbTry != nullptr)
    {
        s_pRegion = (DETOUR_REGION*)pbTry;
        s_pRegion->dwSignature = DETOUR_REGION_SIGNATURE;
        s_pRegion->pMdl  = pMdl;
        s_pRegion->pFree = nullptr;
        s_pRegion->pNext = s_pRegions;
        s_pRegions = s_pRegion;
        DETOUR_TRACE(("  Allocated region %p..%p\n\n",
            s_pRegion, ((PBYTE)s_pRegion) + DETOUR_REGION_SIZE - 1));

        // Put everything but the first trampoline on the free list.
        PBYTE pFree = nullptr;
        pTrampoline = ((PDETOUR_TRAMPOLINE)s_pRegion) + 1;
        for (int i = DETOUR_TRAMPOLINES_PER_REGION - 1; i > 1; i--)
        {
            pTrampoline[i].pbRemain = pFree;
            pFree = (PBYTE)&pTrampoline[i];
        }
        s_pRegion->pFree = (PDETOUR_TRAMPOLINE)pFree;
        goto found_region;
    }

    DETOUR_TRACE(("Couldn't find available memory region!\n"));
    return nullptr;
}

static void detour_free_trampoline(PDETOUR_TRAMPOLINE pTrampoline)
{
    PDETOUR_REGION pRegion = (PDETOUR_REGION)
        ((ULONG_PTR)pTrampoline & ~(ULONG_PTR)(DETOUR_REGION_SIZE - 1));

    memset(pTrampoline, 0, sizeof(*pTrampoline));
    pTrampoline->pbRemain = (PBYTE)pRegion->pFree;
    pRegion->pFree = pTrampoline;
}

static BOOL detour_is_region_empty(PDETOUR_REGION pRegion)
{
    // Stop if the region isn't a region (this would be bad).
    if (pRegion->dwSignature != DETOUR_REGION_SIGNATURE)
    {
        return FALSE;
    }

    PBYTE pbRegionBeg = (PBYTE)pRegion;
    PBYTE pbRegionLim = pbRegionBeg + DETOUR_REGION_SIZE;

    // Stop if any of the trampolines aren't free.
    PDETOUR_TRAMPOLINE pTrampoline = ((PDETOUR_TRAMPOLINE)pRegion) + 1;
    for (int i = 0; i < DETOUR_TRAMPOLINES_PER_REGION; i++)
    {
        if (pTrampoline[i].pbRemain != nullptr &&
            (pTrampoline[i].pbRemain < pbRegionBeg ||
                pTrampoline[i].pbRemain >= pbRegionLim))
        {
            return FALSE;
        }
    }

    // OK, the region is empty.
    return TRUE;
}

static void detour_free_unused_trampoline_regions()
{
    PDETOUR_REGION *ppRegionBase = &s_pRegions;
    PDETOUR_REGION pRegion = s_pRegions;

    while (pRegion != nullptr)
    {
        if (detour_is_region_empty(pRegion))
        {
            *ppRegionBase = pRegion->pNext;

            PMDL pMdl = pRegion->pMdl;
            MmFreePagesFromMdl(pMdl);
            ExFreePool(pMdl);

            s_pRegion = nullptr;
        }
        else
        {
            ppRegionBase = &pRegion->pNext;
        }
        pRegion = *ppRegionBase;
    }
}

static PMDL detour_remap_address(_In_ void* va, _In_ unsigned long size, _Out_ void** new_va)
{
    PMDL  Mdl   = nullptr;
    void* NewVA = nullptr;

    for (;;)
    {
        Mdl = IoAllocateMdl(va, size, FALSE, FALSE, nullptr);
        if (Mdl == nullptr)
        {
            DetoursSetLastError(DETOURS_STATUS_INSUFFICIENT_RESOURCES);
            break;
        }

        __try
        {
            MmProbeAndLockPages(Mdl, KernelMode, IoModifyAccess);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            IoFreeMdl(Mdl), Mdl = nullptr;
            DetoursSetLastError(GetExceptionCode());
            break;
        }

        NewVA = MmGetSystemAddressForMdlSafe(Mdl, HighPagePriority);
        if (NewVA == nullptr)
        {
            MmUnlockPages(Mdl);
            IoFreeMdl(Mdl), Mdl = nullptr;

            DetoursSetLastError(DETOURS_STATUS_INSUFFICIENT_RESOURCES);
            break;
        }

        *new_va = NewVA;
        break;
    }
    
    return Mdl;
}

static void detour_unmap_address(_In_ PMDL mdl)
{
    if (mdl)
    {
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
    }
}

struct DetourIpiContext
{
    volatile LONG           count;
    void*                   context;
    ULONG_PTR(DETOURS_CALLBACK *callback)(DetourIpiContext*, void*);
};

void detour_ipi_signal_done(DetourIpiContext* ipi_ctx)
{
    InterlockedDecrement(&ipi_ctx->count);
}

void detour_ipi_wait_all(DetourIpiContext* ipi_ctx)
{
    while (InterlockedCompareExchange(&ipi_ctx->count, 0, 0))
    {
        YieldProcessor();
    }
}

void detour_ipi_call(ULONG_PTR(DETOURS_CALLBACK *callback)(DetourIpiContext* ipi_ctx, void* ctx), void* ctx)
{
    DetourIpiContext ipi_ctx;

    ipi_ctx.count   = KeQueryActiveProcessorCountEx(0);
    ipi_ctx.context = ctx;
    ipi_ctx.callback= callback;

    KeIpiGenericCall([](ULONG_PTR ctx) 
        -> ULONG_PTR
    {
        DetourIpiContext* ipi_ctx = (DetourIpiContext*)ctx;
        return ipi_ctx->callback(ipi_ctx, ipi_ctx->context);

    }, (ULONG_PTR)&ipi_ctx);
}

///////////////////////////////////////////////////////// Transaction Structs.
//
struct DetourThread
{
    DetourThread *      pNext;
    HANDLE              hThread;
};

struct DetourOperation
{
    DetourOperation *   pNext;
    BOOL                fIsRemove;
    PBYTE *             ppbPointer;
    PBYTE               pbTarget;
    PBYTE               pbTargetEditable;
    PMDL                pbTargetMdl;
    PDETOUR_TRAMPOLINE  pTrampoline;
};

static BOOL                 s_fIgnoreTooSmall       = FALSE;
static BOOL                 s_fRetainRegions        = FALSE;

static LONG                 s_nPendingThreadId      = 0; // Thread owning pending transaction.
static LONG                 s_nPendingError         = DETOURS_STATUS_SUCCESS;
static PVOID *              s_ppPendingError        = nullptr;
static DetourThread *       s_pPendingThreads       = nullptr;
static DetourOperation *    s_pPendingOperations    = nullptr;

//////////////////////////////////////////////////////////////////////////////
//
PVOID DETOURS_API DetourCodeFromPointer(_In_ PVOID pPointer,
    _Out_opt_ PVOID *ppGlobals)
{
    return detour_skip_jmp((PBYTE)pPointer, ppGlobals);
}

//////////////////////////////////////////////////////////// Transaction APIs.
//
BOOL DETOURS_API DetourSetIgnoreTooSmall(_In_ BOOL fIgnore)
{
    BOOL fPrevious = s_fIgnoreTooSmall;
    s_fIgnoreTooSmall = fIgnore;
    return fPrevious;
}

BOOL DETOURS_API DetourSetRetainRegions(_In_ BOOL fRetain)
{
    BOOL fPrevious = s_fRetainRegions;
    s_fRetainRegions = fRetain;
    return fPrevious;
}

PVOID DETOURS_API DetourSetSystemRegionLowerBound(_In_ PVOID pSystemRegionLowerBound)
{
    PVOID pPrevious = s_pSystemRegionLowerBound;
    s_pSystemRegionLowerBound = pSystemRegionLowerBound;
    return pPrevious;
}

PVOID DETOURS_API DetourSetSystemRegionUpperBound(_In_ PVOID pSystemRegionUpperBound)
{
    PVOID pPrevious = s_pSystemRegionUpperBound;
    s_pSystemRegionUpperBound = pSystemRegionUpperBound;
    return pPrevious;
}

LONG DETOURS_API DetourTransactionBegin()
{
    // Only one transaction is allowed at a time.
    _Benign_race_begin_
        if (s_nPendingThreadId != 0)
        {
            return DETOURS_STATUS_INVALID_OPERATION;
        }
    _Benign_race_end_

        // Make sure only one thread can start a transaction.
        if (InterlockedCompareExchange(&s_nPendingThreadId, (LONG)DetoursCurrentThreadId(), 0) != 0)
        {
            return DETOURS_STATUS_INVALID_OPERATION;
        }

    s_pPendingOperations    = nullptr;
    s_pPendingThreads       = nullptr;
    s_ppPendingError        = nullptr;

    // Make sure the trampoline pages are writable.
    s_nPendingError = detour_writable_trampoline_regions();

    return s_nPendingError;
}

LONG DETOURS_API DetourTransactionAbort()
{
    if (s_nPendingThreadId != (LONG)DetoursCurrentThreadId())
    {
        return DETOURS_STATUS_INVALID_OPERATION;
    }

    // Restore all of the page permissions.
    for (DetourOperation *o = s_pPendingOperations; o != nullptr;)
    {
        // We don't care if this fails, because the code is still accessible.
        
        detour_unmap_address(o->pbTargetMdl);

        if (!o->fIsRemove)
        {
            if (o->pTrampoline)
            {
                detour_free_trampoline(o->pTrampoline);
                o->pTrampoline = nullptr;
            }
        }

        DetourOperation *n = o->pNext;
        ExFreePoolWithTag(o, DETOURS_TAG);
        o = n;
    }
    s_pPendingOperations = nullptr;

    // Make sure the trampoline pages are no longer writable.
    detour_runnable_trampoline_regions();

    s_pPendingThreads = nullptr;
    s_nPendingThreadId = 0;

    return DETOURS_STATUS_SUCCESS;
}

LONG DETOURS_API DetourTransactionCommit()
{
    return DetourTransactionCommitEx(nullptr);
}

LONG DETOURS_API DetourTransactionCommitEx(_Out_opt_ PVOID **pppFailedPointer)
{
    if (pppFailedPointer != nullptr)
    {
        // Used to get the last error.
        *pppFailedPointer = s_ppPendingError;
    }
    if (s_nPendingThreadId != (LONG)DetoursCurrentThreadId())
    {
        return DETOURS_STATUS_INVALID_OPERATION;
    }

    // If any of the pending operations failed, then we abort the whole transaction.
    if (s_nPendingError != DETOURS_STATUS_SUCCESS)
    {
        DETOUR_BREAK();
        DetourTransactionAbort();
        return s_nPendingError;
    }

    // Common variables.
    DetourOperation *o;
    BOOL freed = FALSE;

    ULONG_PTR cpu_number = KeGetCurrentProcessorNumber();

    detour_ipi_call([] (DetourIpiContext* ipi_ctx, void* ctx) -> ULONG_PTR
    {
        ULONG_PTR cpu_number = (ULONG_PTR)ctx;

        if (cpu_number == KeGetCurrentProcessorNumber())
        {
            // Insert or remove each of the detours.
            
            DetourOperation *o;
            
            for (o = s_pPendingOperations; o != nullptr; o = o->pNext)
            {
                if (o->fIsRemove)
                {
                    memcpy(o->pbTargetEditable, o->pTrampoline->rbRestore, o->pTrampoline->cbRestore);
#ifdef DETOURS_IA64
                    *o->ppbPointer = (PBYTE)o->pTrampoline->ppldTarget;
#endif // DETOURS_IA64

#ifdef DETOURS_X86
                    *o->ppbPointer = o->pbTarget;
#endif // DETOURS_X86

#ifdef DETOURS_X64
                    *o->ppbPointer = o->pbTarget;
#endif // DETOURS_X64

#ifdef DETOURS_ARM
                    *o->ppbPointer = DETOURS_PBYTE_TO_PFUNC(o->pbTarget);
#endif // DETOURS_ARM

#ifdef DETOURS_ARM64
                    *o->ppbPointer = o->pbTarget;
#endif // DETOURS_ARM
                }
                else
                {
                    //DETOUR_TRACE(("detours: pbTramp =%p, pbRemain=%p, pbDetour=%p, cbRestore=%d\n",
                    //    o->pTrampoline,
                    //    o->pTrampoline->pbRemain,
                    //    o->pTrampoline->pbDetour,
                    //    o->pTrampoline->cbRestore));
                    //
                    //DETOUR_TRACE(("detours: pbTarget=%p: "
                    //    "%02x %02x %02x %02x "
                    //    "%02x %02x %02x %02x "
                    //    "%02x %02x %02x %02x [before]\n",
                    //    o->pbTarget,
                    //    o->pbTarget[0], o->pbTarget[1], o->pbTarget[2], o->pbTarget[3],
                    //    o->pbTarget[4], o->pbTarget[5], o->pbTarget[6], o->pbTarget[7],
                    //    o->pbTarget[8], o->pbTarget[9], o->pbTarget[10], o->pbTarget[11]));

#ifdef DETOURS_IA64
                    ((DETOUR_IA64_BUNDLE*)o->pbTargetEditable)
                        ->SetBrl((UINT64)&o->pTrampoline->bAllocFrame);
                    *o->ppbPointer = (PBYTE)&o->pTrampoline->pldTrampoline;
#endif // DETOURS_IA64

#ifdef DETOURS_X64
                    detour_gen_jmp_indirect(o->pTrampoline->rbCodeIn, o->pTrampoline->rbCodeIn, &o->pTrampoline->pbDetour);
                    //PBYTE pbCode = detour_gen_jmp_immediate(o->pbTargetEditable, o->pbTarget, o->pTrampoline->rbCodeIn);
                    PBYTE pbCode = detour_gen_jmp_far(o->pbTargetEditable, o->pTrampoline->rbCodeIn);
                    pbCode = detour_gen_brk(pbCode, o->pbTargetEditable + o->pTrampoline->cbRestore /*o->pTrampoline->pbRemain*/);
                    *o->ppbPointer = o->pTrampoline->rbCode;
                    UNREFERENCED_PARAMETER(pbCode);
#endif // DETOURS_X64

#ifdef DETOURS_X86
                    PBYTE pbCode = detour_gen_jmp_immediate(o->pbTargetEditable, o->pbTarget, o->pTrampoline->pbDetour);
                    pbCode = detour_gen_brk(pbCode, o->pbTargetEditable + o->pTrampoline->cbRestore /*o->pTrampoline->pbRemain*/);
                    *o->ppbPointer = o->pTrampoline->rbCode;
                    UNREFERENCED_PARAMETER(pbCode);
#endif // DETOURS_X86

#ifdef DETOURS_ARM
                    PBYTE pbCode = detour_gen_jmp_immediate(o->pbTargetEditable, nullptr, o->pTrampoline->pbDetour);
                    pbCode = detour_gen_brk(pbCode, o->pbTargetEditable + o->pTrampoline->cbRestore /*o->pTrampoline->pbRemain*/);
                    *o->ppbPointer = DETOURS_PBYTE_TO_PFUNC(o->pTrampoline->rbCode);
                    UNREFERENCED_PARAMETER(pbCode);
#endif // DETOURS_ARM

#ifdef DETOURS_ARM64
                    PBYTE pbCode = detour_gen_jmp_immediate(o->pbTargetEditable, nullptr, o->pTrampoline->pbDetour);
                    pbCode = detour_gen_brk(pbCode, o->pbTargetEditable + o->pTrampoline->cbRestore /*o->pTrampoline->pbRemain*/);
                    *o->ppbPointer = o->pTrampoline->rbCode;
                    UNREFERENCED_PARAMETER(pbCode);
#endif // DETOURS_ARM64

                    //DETOUR_TRACE(("detours: pbTarget=%p: "
                    //    "%02x %02x %02x %02x "
                    //    "%02x %02x %02x %02x "
                    //    "%02x %02x %02x %02x [after]\n",
                    //    o->pbTarget,
                    //    o->pbTarget[0], o->pbTarget[1], o->pbTarget[2], o->pbTarget[3],
                    //    o->pbTarget[4], o->pbTarget[5], o->pbTarget[6], o->pbTarget[7],
                    //    o->pbTarget[8], o->pbTarget[9], o->pbTarget[10], o->pbTarget[11]));
                    //
                    //DETOUR_TRACE(("detours: pbTramp =%p: "
                    //    "%02x %02x %02x %02x "
                    //    "%02x %02x %02x %02x "
                    //    "%02x %02x %02x %02x\n",
                    //    o->pTrampoline,
                    //    o->pTrampoline->rbCode[0], o->pTrampoline->rbCode[1],
                    //    o->pTrampoline->rbCode[2], o->pTrampoline->rbCode[3],
                    //    o->pTrampoline->rbCode[4], o->pTrampoline->rbCode[5],
                    //    o->pTrampoline->rbCode[6], o->pTrampoline->rbCode[7],
                    //    o->pTrampoline->rbCode[8], o->pTrampoline->rbCode[9],
                    //    o->pTrampoline->rbCode[10], o->pTrampoline->rbCode[11]));

#ifdef DETOURS_IA64
                    //DETOUR_TRACE(("\n"));
                    //DETOUR_TRACE(("detours:  &pldTrampoline  =%p\n",
                    //    &o->pTrampoline->pldTrampoline));
                    //DETOUR_TRACE(("detours:  &bMovlTargetGp  =%p [%p]\n",
                    //    &o->pTrampoline->bMovlTargetGp,
                    //    o->pTrampoline->bMovlTargetGp.GetMovlGp()));
                    //DETOUR_TRACE(("detours:  &rbCode         =%p [%p]\n",
                    //    &o->pTrampoline->rbCode,
                    //    ((DETOUR_IA64_BUNDLE&)o->pTrampoline->rbCode).GetBrlTarget()));
                    //DETOUR_TRACE(("detours:  &bBrlRemainEip  =%p [%p]\n",
                    //    &o->pTrampoline->bBrlRemainEip,
                    //    o->pTrampoline->bBrlRemainEip.GetBrlTarget()));
                    //DETOUR_TRACE(("detours:  &bMovlDetourGp  =%p [%p]\n",
                    //    &o->pTrampoline->bMovlDetourGp,
                    //    o->pTrampoline->bMovlDetourGp.GetMovlGp()));
                    //DETOUR_TRACE(("detours:  &bBrlDetourEip  =%p [%p]\n",
                    //    &o->pTrampoline->bCallDetour,
                    //    o->pTrampoline->bCallDetour.GetBrlTarget()));
                    //DETOUR_TRACE(("detours:  pldDetour       =%p [%p]\n",
                    //    o->pTrampoline->ppldDetour->EntryPoint,
                    //    o->pTrampoline->ppldDetour->GlobalPointer));
                    //DETOUR_TRACE(("detours:  pldTarget       =%p [%p]\n",
                    //    o->pTrampoline->ppldTarget->EntryPoint,
                    //    o->pTrampoline->ppldTarget->GlobalPointer));
                    //DETOUR_TRACE(("detours:  pbRemain        =%p\n",
                    //    o->pTrampoline->pbRemain));
                    //DETOUR_TRACE(("detours:  pbDetour        =%p\n",
                    //    o->pTrampoline->pbDetour));
                    //DETOUR_TRACE(("\n"));
#endif // DETOURS_IA64
                }
            }
        }

        detour_ipi_signal_done(ipi_ctx);
        detour_ipi_wait_all(ipi_ctx);

        return DETOURS_STATUS_SUCCESS;
    }, (void*)cpu_number);
    

    // Restore all of the page permissions and flush the icache.
    for (o = s_pPendingOperations; o != nullptr;)
    {
        // We don't care if this fails, because the code is still accessible.

        detour_unmap_address(o->pbTargetMdl);

        if (o->fIsRemove && o->pTrampoline)
        {
            detour_free_trampoline(o->pTrampoline);
            o->pTrampoline = nullptr;
            freed = true;
        }

        DetourOperation *n = o->pNext;
        ExFreePoolWithTag(o, DETOURS_TAG);
        o = n;
    }
    s_pPendingOperations = nullptr;

    // Free any trampoline regions that are now unused.
    if (freed && !s_fRetainRegions)
    {
        detour_free_unused_trampoline_regions();
    }

    // Make sure the trampoline pages are no longer writable.
    detour_runnable_trampoline_regions();

    s_pPendingThreads  = nullptr;
    s_nPendingThreadId = 0;

    if (pppFailedPointer != nullptr)
    {
        *pppFailedPointer = s_ppPendingError;
    }

    return s_nPendingError;
}

LONG DETOURS_API DetourUpdateThread(_In_ HANDLE hThread)
{
    // If any of the pending operations failed, then we don't need to do this.
    if (s_nPendingError != DETOURS_STATUS_SUCCESS)
    {
        return s_nPendingError;
    }

    // Silently (and safely) drop any attempt to suspend our own thread.
    if (hThread == DetoursCurrentThread())
    {
        return DETOURS_STATUS_SUCCESS;
    }
    
    return DETOURS_STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////// Transacted APIs.
//
LONG DETOURS_API DetourAttach(_Inout_ PVOID *ppPointer,
    _In_ PVOID pDetour)
{
    return DetourAttachEx(ppPointer, pDetour, nullptr, nullptr, nullptr);
}

LONG DETOURS_API DetourAttachEx(_Inout_ PVOID *ppPointer,
    _In_ PVOID pDetour,
    _Out_opt_ PDETOUR_TRAMPOLINE *ppRealTrampoline,
    _Out_opt_ PVOID *ppRealTarget,
    _Out_opt_ PVOID *ppRealDetour)
{
    LONG error = DETOURS_STATUS_SUCCESS;

    if (ppRealTrampoline != nullptr)
    {
        *ppRealTrampoline = nullptr;
    }
    if (ppRealTarget != nullptr)
    {
        *ppRealTarget = nullptr;
    }
    if (ppRealDetour != nullptr)
    {
        *ppRealDetour = nullptr;
    }
    if (pDetour == nullptr)
    {
        DETOUR_TRACE(("empty detour\n"));
        return DETOURS_STATUS_INVALID_PARAMETER;
    }

    if (s_nPendingThreadId != (LONG)DetoursCurrentThreadId())
    {
        DETOUR_TRACE(("transaction conflict with thread id=%d\n", s_nPendingThreadId));
        return DETOURS_STATUS_INVALID_OPERATION;
    }

    // If any of the pending operations failed, then we don't need to do this.
    if (s_nPendingError != DETOURS_STATUS_SUCCESS)
    {
        DETOUR_TRACE(("pending transaction error=%d\n", s_nPendingError));
        return s_nPendingError;
    }

    if (ppPointer == nullptr)
    {
        DETOUR_TRACE(("ppPointer is null\n"));
        return DETOURS_STATUS_INVALID_HANDLE;
    }
    if (*ppPointer == nullptr)
    {
        error = DETOURS_STATUS_INVALID_HANDLE;
        s_nPendingError = error;
        s_ppPendingError = ppPointer;
        DETOUR_TRACE(("*ppPointer is null (ppPointer=%p)\n", ppPointer));
        DETOUR_BREAK();
        return error;
    }

    PBYTE pbTarget = (PBYTE)*ppPointer;
    PDETOUR_TRAMPOLINE pTrampoline = nullptr;
    DetourOperation *o = nullptr;

#ifdef DETOURS_IA64
    PPLABEL_DESCRIPTOR ppldDetour = (PPLABEL_DESCRIPTOR)pDetour;
    PPLABEL_DESCRIPTOR ppldTarget = (PPLABEL_DESCRIPTOR)pbTarget;
    PVOID pDetourGlobals = nullptr;
    PVOID pTargetGlobals = nullptr;

    pDetour  = (PBYTE)DetourCodeFromPointer(ppldDetour, &pDetourGlobals);
    pbTarget = (PBYTE)DetourCodeFromPointer(ppldTarget, &pTargetGlobals);
    DETOUR_TRACE(("  ppldDetour=%p, code=%p [gp=%p]\n",
        ppldDetour, pDetour, pDetourGlobals));
    DETOUR_TRACE(("  ppldTarget=%p, code=%p [gp=%p]\n",
        ppldTarget, pbTarget, pTargetGlobals));
#else // DETOURS_IA64
    pbTarget = (PBYTE)DetourCodeFromPointer(pbTarget, nullptr);
    pDetour  = DetourCodeFromPointer(pDetour, nullptr);
#endif // !DETOURS_IA64

    // Don't follow a jump if its destination is the target function.
    // This happens when the detour does nothing other than call the target.
    if (pDetour == (PVOID)pbTarget)
    {
        if (s_fIgnoreTooSmall)
        {
            goto stop;
        }
        else
        {
            DETOUR_BREAK();
            goto fail;
        }
    }

    if (ppRealTarget != nullptr)
    {
        *ppRealTarget = pbTarget;
    }
    if (ppRealDetour != nullptr)
    {
        *ppRealDetour = pDetour;
    }

    o = (DetourOperation*)ExAllocatePoolWithTag(NonPagedPool, sizeof(DetourOperation), DETOURS_TAG);
    if (o == nullptr)
    {
        error = DETOURS_STATUS_INSUFFICIENT_RESOURCES;
    fail:
        s_nPendingError = error;
        DETOUR_BREAK();
    stop:
        if (pTrampoline != nullptr)
        {
            detour_free_trampoline(pTrampoline);
            pTrampoline = nullptr;
            if (ppRealTrampoline != nullptr)
            {
                *ppRealTrampoline = nullptr;
            }
        }
        if (o != nullptr)
        {
            ExFreePoolWithTag(o, DETOURS_TAG);
            o = nullptr;
        }
        s_ppPendingError = ppPointer;
        return error;
    }

    pTrampoline = detour_alloc_trampoline(pbTarget);
    if (pTrampoline == nullptr)
    {
        error = DETOURS_STATUS_INSUFFICIENT_RESOURCES;
        DETOUR_BREAK();
        goto fail;
    }

    if (ppRealTrampoline != nullptr)
    {
        *ppRealTrampoline = pTrampoline;
    }

    DETOUR_TRACE(("detours: pbTramp=%p, pDetour=%p\n", pTrampoline, pDetour));

    RtlSecureZeroMemory(pTrampoline->rAlign, sizeof(pTrampoline->rAlign));

    // Determine the number of movable target instructions.
    PBYTE pbSrc  = pbTarget;
    PBYTE pbTrampoline = pTrampoline->rbCode;
#ifdef DETOURS_IA64
    PBYTE pbPool = (PBYTE)(&pTrampoline->bBranchIslands + 1);
#else
    PBYTE pbPool = pbTrampoline + sizeof(pTrampoline->rbCode);
#endif
    ULONG cbTarget = 0;
    ULONG cbJump = SIZE_OF_JMP;
    ULONG nAlign = 0;

#ifdef DETOURS_ARM
    // On ARM, we need an extra instruction when the function isn't 32-bit aligned.
    // Check if the existing code is another detour (or at least a similar
    // "ldr pc, [PC+0]" jump.
    if ((ULONG)pbTarget & 2)
    {
        cbJump += 2;

        ULONG op = fetch_thumb_opcode(pbSrc);
        if (op == 0xbf00)
        {
            op = fetch_thumb_opcode(pbSrc + 2);
            if (op == 0xf8dff000)
            { 
                // LDR PC,[PC]
                *((PUSHORT&)pbTrampoline)++ = *((PUSHORT&)pbSrc)++;
                *((PULONG &)pbTrampoline)++ = *((PULONG &)pbSrc)++;
                *((PULONG &)pbTrampoline)++ = *((PULONG &)pbSrc)++;
                cbTarget = (LONG)(pbSrc - pbTarget);
                // We will fall through the "while" because cbTarget is now >= cbJump.
            }
        }
    }
    else
    {
        ULONG op = fetch_thumb_opcode(pbSrc);
        if (op == 0xf8dff000)
        { 
            // LDR PC,[PC]
            *((PULONG&)pbTrampoline)++ = *((PULONG&)pbSrc)++;
            *((PULONG&)pbTrampoline)++ = *((PULONG&)pbSrc)++;
            cbTarget = (LONG)(pbSrc - pbTarget);
            // We will fall through the "while" because cbTarget is now >= cbJump.
        }
    }
#endif

    while (cbTarget < cbJump)
    {
        PBYTE pbOp = pbSrc;
        LONG lExtra = 0;

        DETOUR_TRACE((" DetourCopyInstruction(%p,%p)\n",
            pbTrampoline, pbSrc));
        pbSrc = (PBYTE)
            DetourCopyInstruction(pbTrampoline, (PVOID*)&pbPool, pbSrc, nullptr, &lExtra);
        DETOUR_TRACE((" DetourCopyInstruction() = %p (%d bytes)\n",
            pbSrc, (int)(pbSrc - pbOp)));
        pbTrampoline += (pbSrc - pbOp) + lExtra;
        cbTarget = (LONG)(pbSrc - pbTarget);
        pTrampoline->rAlign[nAlign].obTarget = cbTarget;
        pTrampoline->rAlign[nAlign].obTrampoline = pbTrampoline - pTrampoline->rbCode;
        nAlign++;

        if (nAlign >= ARRAYSIZE(pTrampoline->rAlign))
        {
            break;
        }

        if (detour_does_code_end_function(pbOp))
        {
            break;
        }
    }

    // Consume, but don't duplicate padding if it is needed and available.
    while (cbTarget < cbJump)
    {
        LONG cFiller = detour_is_code_filler(pbSrc);
        if (cFiller == 0)
        {
            break;
        }

        pbSrc += cFiller;
        cbTarget = (LONG)(pbSrc - pbTarget);
    }

#if DETOUR_DEBUG
    {
        DETOUR_TRACE((" detours: rAlign ["));
        LONG n = 0;
        for (n = 0; n < ARRAYSIZE(pTrampoline->rAlign); n++)
        {
            if (pTrampoline->rAlign[n].obTarget == 0 &&
                pTrampoline->rAlign[n].obTrampoline == 0)
            {
                break;
            }
            DETOUR_TRACE((" %d/%d",
                pTrampoline->rAlign[n].obTarget,
                pTrampoline->rAlign[n].obTrampoline
                ));

        }
        DETOUR_TRACE((" ]\n"));
    }
#endif

    if (cbTarget < cbJump || nAlign > ARRAYSIZE(pTrampoline->rAlign))
    {
        // Too few instructions.

        error = DETOURS_STATUS_OUTOFMEMORY;
        if (s_fIgnoreTooSmall)
        {
            goto stop;
        }
        else
        {
            DETOUR_BREAK();
            goto fail;
        }
    }

    if (pbTrampoline > pbPool)
    {
        error = DETOURS_STATUS_OUTOFMEMORY;
        DETOUR_BREAK();
        goto fail;
    }

    pTrampoline->cbCode     = (BYTE)(pbTrampoline - pTrampoline->rbCode);
    pTrampoline->cbRestore  = (BYTE)cbTarget;
    memcpy(pTrampoline->rbRestore, pbTarget, cbTarget);

#ifdef DETOURS_X64
    cbJump = SIZE_OF_JMP_TO_REMAIN;
#endif

#if !defined(DETOURS_IA64)
    if (cbTarget > sizeof(pTrampoline->rbCode) - cbJump)
    {
        // Too many instructions.
        error = DETOURS_STATUS_OUTOFMEMORY;
        DETOUR_BREAK();
        goto fail;
    }
#endif // !DETOURS_IA64

    pTrampoline->pbRemain = pbTarget + cbTarget;
    pTrampoline->pbDetour = (PBYTE)pDetour;

#ifdef DETOURS_IA64
    pTrampoline->ppldDetour = ppldDetour;
    pTrampoline->ppldTarget = ppldTarget;
    pTrampoline->pldTrampoline.EntryPoint = (UINT64)&pTrampoline->bMovlTargetGp;
    pTrampoline->pldTrampoline.GlobalPointer = (UINT64)pDetourGlobals;

    ((DETOUR_IA64_BUNDLE *)pTrampoline->rbCode)->SetStop();

    pTrampoline->bMovlTargetGp.SetMovlGp((UINT64)pTargetGlobals);
    pTrampoline->bBrlRemainEip.SetBrl((UINT64)pTrampoline->pbRemain);

    // Alloc frame:      alloc r41=ar.pfs,11,0,8,0; mov r40=rp
    pTrampoline->bAllocFrame.wide[0] = 0x00000580164d480c;
    pTrampoline->bAllocFrame.wide[1] = 0x00c4000500000200;
    // save r36, r37, r38.
    pTrampoline->bSave37to39.wide[0] = 0x031021004e019001;
    pTrampoline->bSave37to39.wide[1] = 0x8401280600420098;
    // save r34,r35,r36: adds r47=0,r36; adds r46=0,r35; adds r45=0,r34
    pTrampoline->bSave34to36.wide[0] = 0x02e0210048017800;
    pTrampoline->bSave34to36.wide[1] = 0x84011005a042008c;
    // save gp,r32,r33"  adds r44=0,r33; adds r43=0,r32; adds r42=0,gp ;;
    pTrampoline->bSaveGPto33.wide[0] = 0x02b0210042016001;
    pTrampoline->bSaveGPto33.wide[1] = 0x8400080540420080;
    // set detour GP.
    pTrampoline->bMovlDetourGp.SetMovlGp((UINT64)pDetourGlobals);
    // call detour:      brl.call.sptk.few rp=detour ;;
    pTrampoline->bCallDetour.wide[0] = 0x0000000100000005;
    pTrampoline->bCallDetour.wide[1] = 0xd000001000000000;
    pTrampoline->bCallDetour.SetBrlTarget((UINT64)pDetour);
    // pop frame & gp:   adds gp=0,r42; mov rp=r40,+0;; mov.i ar.pfs=r41
    pTrampoline->bPopFrameGp.wide[0] = 0x4000210054000802;
    pTrampoline->bPopFrameGp.wide[1] = 0x00aa029000038005;
    // return to caller: br.ret.sptk.many rp ;;
    pTrampoline->bReturn.wide[0] = 0x0000000100000019;
    pTrampoline->bReturn.wide[1] = 0x0084000880000200;

    DETOUR_TRACE(("detours: &bMovlTargetGp=%p\n", &pTrampoline->bMovlTargetGp));
    DETOUR_TRACE(("detours: &bMovlDetourGp=%p\n", &pTrampoline->bMovlDetourGp));
#endif // DETOURS_IA64

    pbTrampoline = pTrampoline->rbCode + pTrampoline->cbCode;
#ifdef DETOURS_X64
    pbTrampoline = detour_gen_jmp_indirect(pbTrampoline, pbTrampoline, &pTrampoline->pbRemain);
    pbTrampoline = detour_gen_brk(pbTrampoline, pbPool);
#endif // DETOURS_X64

#ifdef DETOURS_X86
    pbTrampoline = detour_gen_jmp_immediate(pbTrampoline, pbTrampoline, pTrampoline->pbRemain);
    pbTrampoline = detour_gen_brk(pbTrampoline, pbPool);
#endif // DETOURS_X86

#ifdef DETOURS_ARM
    pbTrampoline = detour_gen_jmp_immediate(pbTrampoline, &pbPool, pTrampoline->pbRemain);
    pbTrampoline = detour_gen_brk(pbTrampoline, pbPool);
#endif // DETOURS_ARM

#ifdef DETOURS_ARM64
    pbTrampoline = detour_gen_jmp_immediate(pbTrampoline, &pbPool, pTrampoline->pbRemain);
    pbTrampoline = detour_gen_brk(pbTrampoline, pbPool);
#endif // DETOURS_ARM64

    (void)pbTrampoline;

    PMDL  pbTargetMdl       = nullptr;
    PBYTE pbTargetEditable  = nullptr;

    pbTargetMdl = detour_remap_address(pbTarget, cbTarget, (void**)&pbTargetEditable);
    if (pbTargetMdl == nullptr)
    {
        error = DetoursGetLastError();
        DETOUR_BREAK();
        goto fail;
    }

    DETOUR_TRACE(("detours: pbTarget=%p: "
        "%02x %02x %02x %02x "
        "%02x %02x %02x %02x "
        "%02x %02x %02x %02x\n",
        pbTarget,
        pbTarget[0], pbTarget[1], pbTarget[2], pbTarget[3],
        pbTarget[4], pbTarget[5], pbTarget[6], pbTarget[7],
        pbTarget[8], pbTarget[9], pbTarget[10], pbTarget[11]));
    DETOUR_TRACE(("detours: pbTramp =%p: "
        "%02x %02x %02x %02x "
        "%02x %02x %02x %02x "
        "%02x %02x %02x %02x\n",
        pTrampoline,
        pTrampoline->rbCode[0], pTrampoline->rbCode[1],
        pTrampoline->rbCode[2], pTrampoline->rbCode[3],
        pTrampoline->rbCode[4], pTrampoline->rbCode[5],
        pTrampoline->rbCode[6], pTrampoline->rbCode[7],
        pTrampoline->rbCode[8], pTrampoline->rbCode[9],
        pTrampoline->rbCode[10], pTrampoline->rbCode[11]));

    o->fIsRemove        = FALSE;
    o->ppbPointer       = (PBYTE*)ppPointer;
    o->pTrampoline      = pTrampoline;
    o->pbTarget         = pbTarget;
    o->pbTargetEditable = pbTargetEditable;
    o->pbTargetMdl      = pbTargetMdl;
    o->pNext            = s_pPendingOperations;
    s_pPendingOperations= o;

    return DETOURS_STATUS_SUCCESS;
}

LONG DETOURS_API DetourDetach(_Inout_ PVOID *ppPointer,
    _In_ PVOID pDetour)
{
    LONG error = DETOURS_STATUS_SUCCESS;

    if (s_nPendingThreadId != (LONG)DetoursCurrentThreadId())
    {
        return DETOURS_STATUS_INVALID_OPERATION;
    }

    // If any of the pending operations failed, then we don't need to do this.
    if (s_nPendingError != DETOURS_STATUS_SUCCESS)
    {
        return s_nPendingError;
    }

    if (pDetour == nullptr)
    {
        return DETOURS_STATUS_INVALID_PARAMETER;
    }
    if (ppPointer == nullptr)
    {
        return DETOURS_STATUS_INVALID_HANDLE;
    }
    if (*ppPointer == nullptr)
    {
        error = DETOURS_STATUS_INVALID_HANDLE;
        s_nPendingError = error;
        s_ppPendingError = ppPointer;
        DETOUR_BREAK();
        return error;
    }

    DetourOperation *o = (DetourOperation*)ExAllocatePoolWithTag(NonPagedPool, sizeof(DetourOperation), DETOURS_TAG);
    if (o == nullptr)
    {
        error = DETOURS_STATUS_INSUFFICIENT_RESOURCES;
    fail:
        s_nPendingError = error;
        DETOUR_BREAK();
    stop:
        if (o != nullptr)
        {
            ExFreePoolWithTag(o, DETOURS_TAG);
            o = nullptr;
        }
        s_ppPendingError = ppPointer;
        return error;
    }


#ifdef DETOURS_IA64
    PPLABEL_DESCRIPTOR ppldTrampo = (PPLABEL_DESCRIPTOR)*ppPointer;
    PPLABEL_DESCRIPTOR ppldDetour = (PPLABEL_DESCRIPTOR)pDetour;
    PVOID pDetourGlobals = nullptr;
    PVOID pTrampoGlobals = nullptr;

    pDetour = (PBYTE)DetourCodeFromPointer(ppldDetour, &pDetourGlobals);
    PDETOUR_TRAMPOLINE pTrampoline = (PDETOUR_TRAMPOLINE)
        DetourCodeFromPointer(ppldTrampo, &pTrampoGlobals);
    DETOUR_TRACE(("  ppldDetour=%p, code=%p [gp=%p]\n",
        ppldDetour, pDetour, pDetourGlobals));
    DETOUR_TRACE(("  ppldTrampo=%p, code=%p [gp=%p]\n",
        ppldTrampo, pTrampoline, pTrampoGlobals));


    DETOUR_TRACE(("\n"));
    DETOUR_TRACE(("detours:  &pldTrampoline  =%p\n",
        &pTrampoline->pldTrampoline));
    DETOUR_TRACE(("detours:  &bMovlTargetGp  =%p [%p]\n",
        &pTrampoline->bMovlTargetGp,
        pTrampoline->bMovlTargetGp.GetMovlGp()));
    DETOUR_TRACE(("detours:  &rbCode         =%p [%p]\n",
        &pTrampoline->rbCode,
        ((DETOUR_IA64_BUNDLE&)pTrampoline->rbCode).GetBrlTarget()));
    DETOUR_TRACE(("detours:  &bBrlRemainEip  =%p [%p]\n",
        &pTrampoline->bBrlRemainEip,
        pTrampoline->bBrlRemainEip.GetBrlTarget()));
    DETOUR_TRACE(("detours:  &bMovlDetourGp  =%p [%p]\n",
        &pTrampoline->bMovlDetourGp,
        pTrampoline->bMovlDetourGp.GetMovlGp()));
    DETOUR_TRACE(("detours:  &bBrlDetourEip  =%p [%p]\n",
        &pTrampoline->bCallDetour,
        pTrampoline->bCallDetour.GetBrlTarget()));
    DETOUR_TRACE(("detours:  pldDetour       =%p [%p]\n",
        pTrampoline->ppldDetour->EntryPoint,
        pTrampoline->ppldDetour->GlobalPointer));
    DETOUR_TRACE(("detours:  pldTarget       =%p [%p]\n",
        pTrampoline->ppldTarget->EntryPoint,
        pTrampoline->ppldTarget->GlobalPointer));
    DETOUR_TRACE(("detours:  pbRemain        =%p\n",
        pTrampoline->pbRemain));
    DETOUR_TRACE(("detours:  pbDetour        =%p\n",
        pTrampoline->pbDetour));
    DETOUR_TRACE(("\n"));
#else // !DETOURS_IA64
    PDETOUR_TRAMPOLINE pTrampoline =
        (PDETOUR_TRAMPOLINE)DetourCodeFromPointer(*ppPointer, nullptr);
    pDetour = DetourCodeFromPointer(pDetour, nullptr);
#endif // !DETOURS_IA64

    ////////////////////////////////////// Verify that Trampoline is in place.
    //
    LONG cbTarget  = pTrampoline->cbRestore;
    PBYTE pbTarget = pTrampoline->pbRemain - cbTarget;

    if (cbTarget == 0 || cbTarget > sizeof(pTrampoline->rbCode))
    {
        error = DETOURS_STATUS_INVALID_BLOCK;
        if (s_fIgnoreTooSmall)
        {
            goto stop;
        }
        else
        {
            DETOUR_BREAK();
            goto fail;
        }
    }

    if (pTrampoline->pbDetour != pDetour)
    {
        error = DETOURS_STATUS_INVALID_BLOCK;
        if (s_fIgnoreTooSmall)
        {
            goto stop;
        }
        else
        {
            DETOUR_BREAK();
            goto fail;
        }
    }

    PMDL  pbTargetMdl       = nullptr;
    PBYTE pbTargetEditable  = nullptr;

    pbTargetMdl = detour_remap_address(pbTarget, cbTarget, (void**)&pbTargetEditable);
    if (pbTargetMdl == nullptr)
    {
        error = DetoursGetLastError();
        DETOUR_BREAK();
        goto fail;
    }

    o->fIsRemove        = TRUE;
    o->ppbPointer       = (PBYTE*)ppPointer;
    o->pTrampoline      = pTrampoline;
    o->pbTarget         = pbTarget;
    o->pbTargetEditable = pbTargetEditable;
    o->pbTargetMdl      = pbTargetMdl;
    o->pNext            = s_pPendingOperations;
    s_pPendingOperations= o;

    return DETOURS_STATUS_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////////
//
// Helpers for manipulating page protection.
//

// For reference:
//   PAGE_NOACCESS          0x01
//   PAGE_READONLY          0x02
//   PAGE_READWRITE         0x04
//   PAGE_WRITECOPY         0x08
//   PAGE_EXECUTE           0x10
//   PAGE_EXECUTE_READ      0x20
//   PAGE_EXECUTE_READWRITE 0x40
//   PAGE_EXECUTE_WRITECOPY 0x80
//   PAGE_GUARD             ...
//   PAGE_NOCACHE           ...
//   PAGE_WRITECOMBINE      ...

#define DETOUR_PAGE_EXECUTE_ALL    (PAGE_EXECUTE |              \
                                    PAGE_EXECUTE_READ |         \
                                    PAGE_EXECUTE_READWRITE |    \
                                    PAGE_EXECUTE_WRITECOPY)

#define DETOUR_PAGE_NO_EXECUTE_ALL (PAGE_NOACCESS |             \
                                    PAGE_READONLY |             \
                                    PAGE_READWRITE |            \
                                    PAGE_WRITECOPY)

#define DETOUR_PAGE_ATTRIBUTES     (~(DETOUR_PAGE_EXECUTE_ALL | DETOUR_PAGE_NO_EXECUTE_ALL))

C_ASSERT((DETOUR_PAGE_NO_EXECUTE_ALL << 4) == DETOUR_PAGE_EXECUTE_ALL);

#endif // DetoursKernelMode

//  End of File
//////////////////////////////////////////////////////////////////////////
