// mm_x86.c : implementation of the x86 32-bit protected mode memory model.
//
// (c) Ulf Frisk, 2018-2025
// Author: Ulf Frisk, pcileech@frizk.net
//

#include "mm.h"

#define MMX86_MEMMAP_DISPLAYBUFFER_LINE_LENGTH      70
#define MMX86_PTE_IS_TRANSITION(H, pte, iPML)       ((((pte & 0x0c01) == 0x0800) && (iPML == 1) && (H->vmm.tpSystem == VMM_SYSTEM_WINDOWS_32)) ? ((pte & 0xfffff000) | 0x005) : 0)
#define MMX86_PTE_IS_VALID(pte, iPML)               (pte & 0x01)

/*
* Tries to verify that a loaded page table is correct. If just a bit strange
* bytes/ptes supplied in pb will be altered to look better.
*/
BOOL MmX86_TlbPageTableVerify(_In_ VMM_HANDLE H, _Inout_ PBYTE pb, _In_ QWORD pa, _In_ BOOL fSelfRefReq)
{
    return TRUE;
}

/*
* Iterate over the PD to retrieve uncached PT pages and then commit them to the cache.
*/
VOID MmX86_TlbSpider(_In_ VMM_HANDLE H, _In_ PVMM_PROCESS pProcess)
{
    PVMMOB_CACHE_MEM pObPD = NULL;
    DWORD i, pte;
    POB_SET pObPageSet = NULL;
    if(pProcess->fTlbSpiderDone) { return; }
    if(!(pObPageSet = ObSet_New(H))) { return; }
    pObPD = VmmTlbGetPageTable(H, pProcess->paDTB & 0xfffff000, FALSE);
    if(!pObPD) { goto fail; }
    for(i = 0; i < 1024; i++) {
        pte = pObPD->pdw[i];
        if(!(pte & 0x01)) { continue; }                 // not valid
        if(pte & 0x80) { continue; }                    // not valid ptr to PT
        if(pProcess->fUserOnly && !(pte & 0x04)) { continue; }    // supervisor page when fUserOnly -> not valid
        ObSet_Push(pObPageSet, pte & 0xfffff000);
    }
    VmmTlbPrefetch(H, pObPageSet);
    pProcess->fTlbSpiderDone = TRUE;
fail:
    Ob_DECREF(pObPageSet);
    Ob_DECREF(pObPD);
}

const DWORD MMX86_PAGETABLEMAP_PML_REGION_SIZE[3] = { 0, 12, 22 };

VOID MmX86_MapInitialize_Index(_In_ VMM_HANDLE H, _In_ PVMM_PROCESS pProcess, _In_ PVMM_MAP_PTEENTRY pMemMap, _In_ PDWORD pcMemMap, _In_ DWORD vaBase, _In_ BYTE iPML, _In_ DWORD PTEs[1024], _In_ BOOL fSupervisorPML, _In_ QWORD paMax)
{
    PVMMOB_CACHE_MEM pObNextPT;
    DWORD i, va, pte;
    QWORD cPages;
    BOOL fUserOnly, fNextSupervisorPML, fPagedOut = FALSE;
    PVMM_MAP_PTEENTRY pMemMapEntry = pMemMap + *pcMemMap - 1;
    fUserOnly = pProcess->fUserOnly;
    for(i = 0; i < 1024; i++) {
        pte = PTEs[i];
        if(!MMX86_PTE_IS_VALID(pte, iPML)) {
            if(!pte) { continue; }
            if(iPML != 1) { continue; }
            pte = MMX86_PTE_IS_TRANSITION(H, pte, iPML);
            pte = 0x00000005 | (pte ? (pte & 0xfffff000) : 0);  // GUESS READ-ONLY USER PAGE IF NON TRANSITION
            fPagedOut = TRUE;
        } else {
            fPagedOut = FALSE;
        }
        if((pte & 0xfffff000) > paMax) { continue; }
        if(fSupervisorPML) { pte = pte & 0xfffffffb; }
        if(fUserOnly && !(pte & 0x04)) { continue; }
        va = vaBase + (i << MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
        if((iPML == 1) || (pte & 0x80) /* PS */) {
            if((*pcMemMap == 0) ||
                ((pMemMapEntry->fPage != (pte & VMM_MEMMAP_PAGE_MASK)) && !fPagedOut) ||
                (va != pMemMapEntry->vaBase + (pMemMapEntry->cPages << 12))) {
                if(*pcMemMap + 1 >= VMM_MEMMAP_ENTRIES_MAX) { return; }
                pMemMapEntry = pMemMap + *pcMemMap;
                pMemMapEntry->vaBase = va;
                pMemMapEntry->fPage = pte & VMM_MEMMAP_PAGE_MASK;
                cPages = 1ULL << (MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML] - 12);
                if(fPagedOut) { pMemMapEntry->cSoftware += (DWORD)cPages; }
                pMemMapEntry->cPages = cPages;
                *pcMemMap = *pcMemMap + 1;
                if(*pcMemMap >= VMM_MEMMAP_ENTRIES_MAX - 1) { return; }
                continue;
            }
            cPages = 1ULL << (MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML] - 12);
            if(fPagedOut) { pMemMapEntry->cSoftware += (DWORD)cPages; }
            pMemMapEntry->cPages += cPages;
            continue;
        }
        // maps page table
        fNextSupervisorPML = !(pte & 0x04);
        pObNextPT = VmmTlbGetPageTable(H, pte & 0xfffff000, FALSE);
        if(!pObNextPT) { continue; }
        MmX86_MapInitialize_Index(H, pProcess, pMemMap, pcMemMap, va, 1, pObNextPT->pdw, fNextSupervisorPML, paMax);
        Ob_DECREF(pObNextPT);
        pMemMapEntry = pMemMap + *pcMemMap - 1;
    }
}

VOID MmX86_CallbackCleanup_ObPteMap(PVMMOB_MAP_PTE pOb)
{
    LocalFree(pOb->pbMultiText);
}

_Success_(return)
BOOL MmX86_PteMapInitialize(_In_ VMM_HANDLE H, _In_ PVMM_PROCESS pProcess)
{
    PVMMOB_CACHE_MEM pObPD;
    DWORD cMemMap = 0;
    PVMM_MAP_PTEENTRY pMemMap = NULL;
    PVMMOB_MAP_PTE pObMap = NULL;
    // already existing?
    if(pProcess->Map.pObPte) { TRUE; }
    EnterCriticalSection(&pProcess->LockUpdate);
    if(pProcess->Map.pObPte) {
        LeaveCriticalSection(&pProcess->LockUpdate);
        return TRUE;
    }
    // allocate temporary buffer and walk page tables
    VmmTlbSpider(H, pProcess);
    pObPD = VmmTlbGetPageTable(H, pProcess->paDTB & 0xfffff000, FALSE);
    if(pObPD) {
        pMemMap = (PVMM_MAP_PTEENTRY)LocalAlloc(LMEM_ZEROINIT, VMM_MEMMAP_ENTRIES_MAX * sizeof(VMM_MAP_PTEENTRY));
        if(pMemMap) {
            MmX86_MapInitialize_Index(H, pProcess, pMemMap, &cMemMap, 0, 2, pObPD->pdw, FALSE, H->dev.paMax);
        }
        Ob_DECREF(pObPD);
    }
    // allocate VmmOb depending on result
    pObMap = Ob_AllocEx(H, OB_TAG_MAP_PTE, 0, sizeof(VMMOB_MAP_PTE) + cMemMap * sizeof(VMM_MAP_PTEENTRY), (OB_CLEANUP_CB)MmX86_CallbackCleanup_ObPteMap, NULL);
    if(!pObMap) {
        pProcess->Map.pObPte = Ob_AllocEx(H, OB_TAG_MAP_PTE, LMEM_ZEROINIT, sizeof(VMMOB_MAP_PTE), NULL, NULL);
        LeaveCriticalSection(&pProcess->LockUpdate);
        LocalFree(pMemMap);
        return TRUE;
    }
    pObMap->pbMultiText = NULL;
    pObMap->cbMultiText = 0;
    pObMap->fTagScan = FALSE;
    pObMap->cMap = cMemMap;
    memcpy(pObMap->pMap, pMemMap, cMemMap * sizeof(VMM_MAP_PTEENTRY));
    LocalFree(pMemMap);
    pProcess->Map.pObPte = pObMap;
    LeaveCriticalSection(&pProcess->LockUpdate);
    return TRUE;
}

VOID MmX86_Virt2PhysEx(_In_ VMM_HANDLE H, _In_ PVMM_V2P_ENTRY pV2Ps, _In_ DWORD cV2Ps, _In_ BOOL fUserOnly, _In_ BYTE iPML)
{
    BOOL fValidNextPT = FALSE;
    DWORD pte, i, iV2P;
    PVMM_V2P_ENTRY pV2P;
    if(iPML == (BYTE)-1) { iPML = 2; }
    VmmTlbGetPageTableEx(H, pV2Ps, cV2Ps, FALSE);
    for(iV2P = 0; iV2P < cV2Ps; iV2P++) {
        pV2P = pV2Ps + iV2P;
        pV2P->paPT = 0;
        if(!pV2P->pObPTE) {
            continue;
        }
        if(pV2P->pa) {
            Ob_DECREF_NULL(&pV2P->pObPTE);
            continue;
        }
        i = 0x3ff & (pV2P->va >> MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
        pte = pV2P->pObPTE->pdw[i];
        Ob_DECREF_NULL(&pV2P->pObPTE);
        if(!MMX86_PTE_IS_VALID(pte, iPML)) {
            if(iPML == 1) {
                pV2P->pte = pte;
                pV2P->fPaging = TRUE;
            }
            continue;
        }
        if(fUserOnly && !(pte & 0x04)) { continue; }            // SUPERVISOR PAGE & USER MODE REQ
        if(iPML == 1) {     // 4kB PAGE:
            pV2P->pa = pte & 0xfffff000;
            pV2P->fPhys = TRUE;
            continue;
        }
        if(pte & 0x80) {    // 4MB PAGE:
            if(pte & 0x003e0000) { continue; }                  // RESERVED
            pV2P->pa = (((QWORD)(pte & 0x0001e000)) << (32 - 13)) + (pte & 0xffc00000) + (pV2P->va & 0x003ff000);
            pV2P->fPhys = TRUE;
            continue;
        }
        // PD ENTRY:
        pV2P->paPT = pte & 0xfffff000;
        fValidNextPT = TRUE;
    }
    if(fValidNextPT && (iPML == 2)) {
        MmX86_Virt2PhysEx(H, pV2Ps, cV2Ps, fUserOnly, 1);
    }
}

_Success_(return)
BOOL MmX86_Virt2Phys(_In_ VMM_HANDLE H, _In_ QWORD paPT, _In_ BOOL fUserOnly, _In_ BYTE iPML, _In_ QWORD va, _Out_ PQWORD ppa)
{
    DWORD pte, i;
    PVMMOB_CACHE_MEM pObPTEs;
    //PBYTE pbPTEs;
    if(va > 0xffffffff) { return FALSE; }
    if(paPT > 0xffffffff) { return FALSE; }
    if(iPML == (BYTE)-1) { iPML = 2; }
    pObPTEs = VmmTlbGetPageTable(H, paPT & 0xfffff000, FALSE);
    if(!pObPTEs) { return FALSE; }
    i = 0x3ff & (va >> MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
    pte = pObPTEs->pdw[i];
    Ob_DECREF(pObPTEs);
    if(!MMX86_PTE_IS_VALID(pte, iPML)) {
        if(iPML == 1) { *ppa = pte; }                       // NOT VALID
        return FALSE;
    }
    if(fUserOnly && !(pte & 0x04)) { return FALSE; }        // SUPERVISOR PAGE & USER MODE REQ
    if((iPML == 2) && !(pte & 0x80) /* PS */) {
        return MmX86_Virt2Phys(H, pte, fUserOnly, 1, va, ppa);
    }
    if(iPML == 1) { // 4kB PAGE
        *ppa = pte & 0xfffff000;
        return TRUE;
    }
    // 4MB PAGE
    if(pte & 0x003e0000) { return FALSE; }                  // RESERVED
    *ppa = (((QWORD)(pte & 0x0001e000)) << (32 - 13)) + (pte & 0xffc00000) + (va & 0x003ff000);
    return TRUE;
}

VOID MmX86_Virt2PhysVadEx(_In_ VMM_HANDLE H, _In_ QWORD paPT, _Inout_ PVMMOB_MAP_VADEX pVadEx, _In_ BYTE iPML, _Inout_ PDWORD piVadEx)
{
    BYTE flags;
    PVMM_MAP_VADEXENTRY peVadEx;
    DWORD pte, iPte, iVadEx;
    PVMMOB_CACHE_MEM pObPTEs = NULL;
    if(iPML == (BYTE)-1) { iPML = 2; }
    if((pVadEx->pMap[*piVadEx].va > 0xffffffff) || (paPT > 0xffffffff) || !(pObPTEs = VmmTlbGetPageTable(H, paPT & 0xfffff000, FALSE))) {
        *piVadEx = *piVadEx + 1;
        return;
    }
next_entry:
    iVadEx = *piVadEx;
    peVadEx = &pVadEx->pMap[iVadEx];
    peVadEx->flags = 0;
    iPte = 0x3ff & (peVadEx->va >> MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
    pte = pObPTEs->pdw[iPte];
    if(!MMX86_PTE_IS_VALID(pte, iPML)) { goto next_check; } // NOT VALID
    if(!(pte & 0x04)) { goto next_check; }                  // SUPERVISOR PAGE & USER MODE REQ
    if((iPML == 2) && !(pte & 0x80) /* PS */) {
        MmX86_Virt2PhysVadEx(H, pte, pVadEx, 1, piVadEx);
        Ob_DECREF(pObPTEs);
        return;
    }
    flags = VADEXENTRY_FLAG_HARDWARE;
    flags |= (pte & VMM_MEMMAP_PAGE_W) ? VADEXENTRY_FLAG_W : 0;
    flags |= (pte & VMM_MEMMAP_PAGE_NS) ? 0 : VADEXENTRY_FLAG_K;
    if(iPML == 1) {                     // 4kB PAGE
        peVadEx->flags = flags;
        peVadEx->pa = pte & 0xfffff000;
        peVadEx->tp = VMM_PTE_TP_HARDWARE;
    } else if(!(pte & 0x003e0000)) {    // 4MB PAGE
        peVadEx->flags = flags;
        peVadEx->pa = (((QWORD)(pte & 0x0001e000)) << (32 - 13)) + (pte & 0xffc00000) + (peVadEx->va & 0x003ff000);
        peVadEx->tp = VMM_PTE_TP_HARDWARE;
    }
next_check:
    peVadEx->pte = pte;
    peVadEx->iPML = iPML;
    *piVadEx = *piVadEx + 1;
    if((iPML == 1) && (iPte < 0x3ff) && (iVadEx + 1 < pVadEx->cMap) && (peVadEx->va + 0x1000 == pVadEx->pMap[iVadEx + 1].va)) { goto next_entry; }
    Ob_DECREF(pObPTEs);
}

VOID MmX86_Virt2PhysGetInformation_DoWork(_In_ VMM_HANDLE H, _Inout_ PVMM_PROCESS pProcess, _Inout_ PVMM_VIRT2PHYS_INFORMATION pVirt2PhysInfo, _In_ BYTE iPML, _In_ QWORD paPT)
{
    PVMMOB_CACHE_MEM pObPTEs;
    DWORD pte, i;
    pObPTEs = VmmTlbGetPageTable(H, paPT, FALSE);
    if(!pObPTEs) { return; }
    i = 0x3ff & (pVirt2PhysInfo->va >> MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
    pte = pObPTEs->pdw[i];
    Ob_DECREF(pObPTEs);
    pVirt2PhysInfo->pas[iPML] = paPT;
    pVirt2PhysInfo->iPTEs[iPML] = (WORD)i;
    pVirt2PhysInfo->PTEs[iPML] = pte;
    if(!MMX86_PTE_IS_VALID(pte, iPML)) { return; }          // NOT VALID
    if(pProcess->fUserOnly && !(pte & 0x04)) { return; }    // SUPERVISOR PAGE & USER MODE REQ
    if(iPML == 1) {     // 4kB page
        pVirt2PhysInfo->pas[0] = pte & 0xfffff000;
        return;
    }
    if(pte & 0x80) {    // 4MB page
        if(pte & 0x003e0000) { return; }                    // RESERVED
        pVirt2PhysInfo->pas[0] = (pte & 0xffc00000) + (((QWORD)(pte & 0x0001e000)) << (32 - 13));
        return;
    }
    MmX86_Virt2PhysGetInformation_DoWork(H, pProcess, pVirt2PhysInfo, 1, pte & 0xfffff000); // PDE
}

VOID MmX86_Virt2PhysGetInformation(_In_ VMM_HANDLE H, _Inout_ PVMM_PROCESS pProcess, _Inout_ PVMM_VIRT2PHYS_INFORMATION pVirt2PhysInfo)
{
    QWORD va;
    if(pVirt2PhysInfo->va > 0xffffffff) { return; }
    va = pVirt2PhysInfo->va;
    ZeroMemory(pVirt2PhysInfo, sizeof(VMM_VIRT2PHYS_INFORMATION));
    pVirt2PhysInfo->tpMemoryModel = VMM_MEMORYMODEL_X86;
    pVirt2PhysInfo->va = va;
    MmX86_Virt2PhysGetInformation_DoWork(H, pProcess, pVirt2PhysInfo, 2, pProcess->paDTB & 0xfffff000);
}

VOID MmX86_Phys2VirtGetInformation_Index(_In_ VMM_HANDLE H, _In_ PVMM_PROCESS pProcess, _In_ DWORD vaBase, _In_ BYTE iPML, _In_ DWORD PTEs[1024], _In_ QWORD paMax, _Inout_ PVMMOB_PHYS2VIRT_INFORMATION pP2V)
{
    BOOL fUserOnly;
    QWORD pa;
    DWORD i, va, pte;
    PVMMOB_CACHE_MEM pObNextPT;
    if(!pProcess->fTlbSpiderDone) {
        VmmTlbSpider(H, pProcess);
    }
    fUserOnly = pProcess->fUserOnly;
    for(i = 0; i < 1024; i++) {
        pte = PTEs[i];
        if(!MMX86_PTE_IS_VALID(pte, iPML)) { continue; }
        if((pte & 0xfffff000) > paMax) { continue; }
        if(fUserOnly && !(pte & 0x04)) { continue; }
        va = vaBase + (i << MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
        if(iPML == 1) {
            if((pte & 0xfffff000) == (pP2V->paTarget & 0xfffff000)) {
                pP2V->pvaList[pP2V->cvaList] = va | (pP2V->paTarget & 0xfff);
                pP2V->cvaList++;
                if(pP2V->cvaList == VMM_PHYS2VIRT_INFORMATION_MAX_PROCESS_RESULT) { return; }
            }
            continue;
        }
        if(pte & 0x80 /* PS */) {
            pa = ((pte & 0xffc00000) | ((QWORD)(pte & 0x001fe000) << 19));
            if(pa == (pP2V->paTarget & 0xffc00000)) {
                pP2V->pvaList[pP2V->cvaList] = va | (pP2V->paTarget & 0x3fffff);
                pP2V->cvaList++;
                if(pP2V->cvaList == VMM_PHYS2VIRT_INFORMATION_MAX_PROCESS_RESULT) { return; }
            }
            continue;
        }
        // maps page table
        if(fUserOnly && !(pte & 0x04)) { continue; }    // do not go into supervisor pages if user-only adderss space
        pObNextPT = VmmTlbGetPageTable(H, pte & 0xfffff000, FALSE);
        if(!pObNextPT) { continue; }
        MmX86_Phys2VirtGetInformation_Index(H, pProcess, va, 1, pObNextPT->pdw, paMax, pP2V);
        Ob_DECREF(pObNextPT);
        if(pP2V->cvaList == VMM_PHYS2VIRT_INFORMATION_MAX_PROCESS_RESULT) { return; }
    }
}

VOID MmX86_Phys2VirtGetInformation(_In_ VMM_HANDLE H, _In_ PVMM_PROCESS pProcess, _Inout_ PVMMOB_PHYS2VIRT_INFORMATION pP2V)
{
    PVMMOB_CACHE_MEM pObPD;
    if((pP2V->cvaList == VMM_PHYS2VIRT_INFORMATION_MAX_PROCESS_RESULT) || (pP2V->paTarget > H->dev.paMax)) { return; }
    pObPD = VmmTlbGetPageTable(H, pProcess->paDTB & 0xfffff000, FALSE);
    if(!pObPD) { return; }
    MmX86_Phys2VirtGetInformation_Index(H, pProcess, 0, 2, pObPD->pdw, H->dev.paMax, pP2V);
    Ob_DECREF(pObPD);
}

VOID MmX86_Close(_In_ VMM_HANDLE H)
{
    H->vmm.f32 = FALSE;
    H->vmm.tpMemoryModel = VMM_MEMORYMODEL_NA;
    ZeroMemory(&H->vmm.fnMemoryModel, sizeof(VMM_MEMORYMODEL_FUNCTIONS));
}

VOID MmX86_Initialize(_In_ VMM_HANDLE H)
{
    PVMM_MEMORYMODEL_FUNCTIONS pfnsMemoryModel = &H->vmm.fnMemoryModel;
    if(pfnsMemoryModel->pfnClose) {
        pfnsMemoryModel->pfnClose(H);
    }
    pfnsMemoryModel->pfnClose = MmX86_Close;
    pfnsMemoryModel->pfnVirt2Phys = MmX86_Virt2Phys;
    pfnsMemoryModel->pfnVirt2PhysEx = MmX86_Virt2PhysEx;
    pfnsMemoryModel->pfnVirt2PhysVadEx = MmX86_Virt2PhysVadEx;
    pfnsMemoryModel->pfnVirt2PhysGetInformation = MmX86_Virt2PhysGetInformation;
    pfnsMemoryModel->pfnPhys2VirtGetInformation = MmX86_Phys2VirtGetInformation;
    pfnsMemoryModel->pfnPteMapInitialize = MmX86_PteMapInitialize;
    pfnsMemoryModel->pfnTlbSpider = MmX86_TlbSpider;
    pfnsMemoryModel->pfnTlbPageTableVerify = MmX86_TlbPageTableVerify;
    H->vmm.tpMemoryModel = VMM_MEMORYMODEL_X86;
    H->vmm.f32 = TRUE;
}
