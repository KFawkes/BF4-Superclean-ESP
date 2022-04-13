#include "utils.h"

unsigned __int64 FindPattern(char* module, const char* pattern)
{
	MODULEINFO moduleInfo;
	if (!GetModuleInformation((HANDLE)-1, GetModuleHandle(module), &moduleInfo, sizeof(MODULEINFO)) || !moduleInfo.lpBaseOfDll)
		return NULL;

	__int64 range_start = (__int64)moduleInfo.lpBaseOfDll;
	__int64 range_end = (__int64)moduleInfo.lpBaseOfDll + moduleInfo.SizeOfImage;

	const char* pat = pattern;
	unsigned __int64 firstMatch = NULL;
	unsigned __int64 pCur = range_start;
	unsigned __int64 region_end;
	MEMORY_BASIC_INFORMATION mbi{};
	while (VirtualQuery((LPCVOID)pCur, &mbi, sizeof(mbi)) == sizeof(mbi)) {
		if (pCur >= range_end - strlen(pattern))
			break;

		if (!(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) || !(mbi.State & MEM_COMMIT)) {
			pCur += mbi.RegionSize;
			continue;
		}
		else
			region_end = pCur + mbi.RegionSize;

		firstMatch = 0;
		while (pCur < region_end)
		{
			if (!*pat)
				return firstMatch;
			if (*(PBYTE)pat == '\?' || *(PBYTE)pCur == getByte(pat)) {
				if (!firstMatch)
					firstMatch = pCur;
				if (!pat[1] || !pat[2])
					return firstMatch;

				if (*(PWORD)pat == '\?\?' || *(PBYTE)pat != '\?')
					pat += 3;
				else
					pat += 2;
			}
			else {
				if (firstMatch)
					pCur = firstMatch;
				pat = pattern;
				firstMatch = 0;
			}
			pCur++;
		}
	}
	return NULL;
}

PBYTE HookVTableFunction(PDWORD64* ppVTable, PBYTE pHook, SIZE_T iIndex)
{
	DWORD dwOld = 0;
	VirtualProtect((void*)((*ppVTable) + iIndex), sizeof(PDWORD64), PAGE_EXECUTE_READWRITE, &dwOld);

	PBYTE pOrig = ((PBYTE)(*ppVTable)[iIndex]);
	(*ppVTable)[iIndex] = (DWORD64)pHook;
	VirtualProtect((void*)((*ppVTable) + iIndex), sizeof(PDWORD64), dwOld, &dwOld);

	return pOrig;
}