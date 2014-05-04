/*
    EasyHook - The reinvention of Windows API hooking
 
    Copyright (C) 2009 Christoph Husse

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

    Please visit http://www.codeplex.com/easyhook for more information
    about the project and latest updates.
*/
#include "stdafx.h"
#include <mscoree.h>
#include <metahost.h>
#if _DEBUG
#include <atltrace.h>
#endif

// prevent static NET binding...
typedef HRESULT __stdcall PROC_CorBindToRuntime(
                    LPCWSTR pwszVersion,
                    LPCWSTR pwszBuildFlavor,
                    REFCLSID rclsid,
                    REFIID riid,
                    LPVOID FAR *ppv);
typedef HRESULT __stdcall PROC_CLRCreateInstance(
    REFCLSID  clsid,
    REFIID riid,
    LPVOID * ppInterface
);

// a macro to compress error information...
#define UNMANAGED_ERROR(code) {ErrorCode = ((code) & 0xFF) | 0xF0000000; goto ABORT_ERROR;}

// a macro that outputs the message to the Debug console if a debug build
void DEBUGOUT(const char * fmt, ...) 
{ 
#if _DEBUG
    va_list ap;
    va_start(ap, fmt);
    ATLTRACE2(fmt, ap);
    ATLTRACE("\n");
    va_end(ap);
#endif
}

EASYHOOK_NT_INTERNAL CompleteUnmanagedInjection(LPREMOTE_INFO InInfo)
{
/*
Description:

    Loads the user library and finally executes the user entry point.

*/
    ULONG		            ErrorCode = 0;
    HMODULE                 hUserLib = LoadLibraryW(InInfo->UserLibrary);
    REMOTE_ENTRY_INFO       EntryInfo;
    REMOTE_ENTRY_POINT*     EntryProc = (REMOTE_ENTRY_POINT*)GetProcAddress(
                                hUserLib,
#ifdef _M_X64
                                "NativeInjectionEntryPoint");
#else
                                "_NativeInjectionEntryPoint@4");
#endif

    if(hUserLib == NULL)
        UNMANAGED_ERROR(20);

    if(EntryProc == NULL)
        UNMANAGED_ERROR(21);

    // set and close event
    if(!SetEvent(InInfo->hRemoteSignal))
        UNMANAGED_ERROR(22);

    // invoke user defined entry point
    EntryInfo.HostPID = InInfo->HostProcess;
    EntryInfo.UserData = (InInfo->UserDataSize) ? InInfo->UserData : NULL;
    EntryInfo.UserDataSize = InInfo->UserDataSize;

    EntryProc(&EntryInfo);

    return ERROR_SUCCESS;

ABORT_ERROR:

    return ErrorCode;
}

EASYHOOK_NT_INTERNAL CompleteManagedInjection(LPREMOTE_INFO InInfo)
{
/*
Description:

    Loads the NET runtime into the calling process and invokes the
    managed injection entry point (EasyHook.InjectionLoader.Main).
*/
    ICLRMetaHost*           MetaClrHost = NULL;
    ICLRRuntimeInfo*        RuntimeInfo = NULL;
    ICLRRuntimeHost*        RuntimeClrHost = NULL;

	DWORD					ErrorCode = 0;
    WCHAR                   ParamString[MAX_PATH];
    REMOTE_ENTRY_INFO       EntryInfo;
    HMODULE                 hMsCorEE = LoadLibraryA("mscoree.dll");
    PROC_CorBindToRuntime*  CorBindToRuntime = (PROC_CorBindToRuntime*)GetProcAddress(hMsCorEE, "CorBindToRuntime"); // .NET 2.0/3.5 framework creation method
    PROC_CLRCreateInstance* CLRCreateInstance = (PROC_CLRCreateInstance*)GetProcAddress(hMsCorEE, "CLRCreateInstance"); // .NET 4.0+ framework creation method
    DWORD                   RetVal;
    bool                    UseCorBindToRuntime = false;

    if(CorBindToRuntime == NULL && CLRCreateInstance == NULL)
        UNMANAGED_ERROR(10); // mscoree.dll does not exist or does not expose either of the framework creation methods

    UseCorBindToRuntime = CLRCreateInstance == NULL;

    // invoke user defined entry point
    EntryInfo.HostPID = InInfo->HostProcess;
    EntryInfo.UserData = InInfo->UserData;
    EntryInfo.UserDataSize = InInfo->UserDataSize;

    if (!UseCorBindToRuntime)
    {
        // Attempt to use .NET 3.5/4 runtime object creation method rather than deprecated .NET 2.0 CorBindToRuntime
        if (FAILED(CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (LPVOID*)&MetaClrHost)))
        {
            // Failed to get a MetaHost instance
            DEBUGOUT("Failed to retrieve CLRMetaHost instance");
            UseCorBindToRuntime = true;
        }
        else
        {
            /*
                It is possible to create a ICLRRuntimeInfo object based on the runtime required by InInfo->UserLibrary
                however this requires that the assembly containing the "EasyHook.InjectionLoader" (usually EasyHook.dll)
                must be targetting the same framework version as the assembly(ies) to be injected. This is because the
                first CLR assembly injected into the target process in this case is usually the EasyHook.dll assembly.

                So instead we are providing a specific .NET version (for now v4.0.30319), this will need to be
                passed as a parameter in the future.
            */
            // TODO: add documentation about what happens when injecting into a managed process where the .NET framework is already loaded
            LPCWSTR frameworkVersion = L"v4.0.30319"; // TODO: .NET version string to be passed in "InInfo"
            if (FAILED(MetaClrHost->GetRuntime(
                frameworkVersion, 
                IID_ICLRRuntimeInfo,
                (LPVOID*)&RuntimeInfo)))
            {
                // .NET version requested is not available
                DEBUGOUT("Failed to retrieve runtime info for framework version: %s", frameworkVersion);
                UseCorBindToRuntime = true;
            }
            else
            {
                if (FAILED(RuntimeInfo->GetInterface(CLSID_CLRRuntimeHost, IID_ICLRRuntimeHost, (LPVOID*)&RuntimeClrHost)))
                {
                    // Failed to create the requested CLR host version
                    // TODO: add documentation about why this might happen - e.g. does this happen if an older framework version is already loaded by the target?
                    DEBUGOUT("Failed to create CLR host for framework version: %s", frameworkVersion);
                    UseCorBindToRuntime = true;
                }
                else
                {
                    RuntimeClrHost->Start();

                    // Enable support for running older .NET v2 assemblies within .NET v4 (e.g. EasyHook.dll is .NET 2.0)
	                if (FAILED(RuntimeInfo->BindAsLegacyV2Runtime()))
                        DEBUGOUT("Unable to BindAsLegacyV2Runtime");
                }
            }
        }
    }

    if (UseCorBindToRuntime)
    {
        // load NET-Runtime and execute user defined method
        if(!RTL_SUCCESS(CorBindToRuntime(NULL, NULL, CLSID_CLRRuntimeHost, IID_ICLRRuntimeHost, (void**)&RuntimeClrHost)))
        {
            DEBUGOUT("CorBindToRuntime failed");
            UNMANAGED_ERROR(11);
        }
        
        RuntimeClrHost->Start();
    }

    /*
        Test library code.
        This is because if once we have set the remote signal, there is no way to
        notify the host about errors. If the following call succeeds, then it will
        also do so some lines later... If not, then we are still able to report an error.

        The EasyHook managed injection loader will establish a connection to the
        host, so that further error reporting is still possible after we set the event!
    */
    RtlLongLongToUnicodeHex((LONGLONG)&EntryInfo, ParamString);

    if(!RTL_SUCCESS(RuntimeClrHost->ExecuteInDefaultAppDomain(
            InInfo->UserLibrary,
            L"EasyHook.InjectionLoader",
            L"Main",
            ParamString,
            &RetVal)))
    {
        if (UseCorBindToRuntime)
            UNMANAGED_ERROR(12); // We already tried the CorBindToRuntime method and it has failed

        // Running the assembly in the .NET 4.0 Runtime did not work;
        // Stop and attempt to run it in the .NET 2.0/3.5 Runtime
        if(RuntimeClrHost != NULL)
            RuntimeClrHost->Release();
        
        if(!RTL_SUCCESS(CorBindToRuntime(NULL, NULL, CLSID_CLRRuntimeHost, IID_ICLRRuntimeHost, (void**)&RuntimeClrHost)))
            UNMANAGED_ERROR(11);
        
        RuntimeClrHost->Start();
        
        RtlLongLongToUnicodeHex((LONGLONG)&EntryInfo, ParamString);
        if(!RTL_SUCCESS(RuntimeClrHost->ExecuteInDefaultAppDomain(InInfo->UserLibrary, L"EasyHook.InjectionLoader", L"Main", ParamString, &RetVal)))
            UNMANAGED_ERROR(14); // Execution under both .NET 4 and .NET 2/3.5 failed (new ErrorCode: 14, for EasyHook 2.7)
    }

    if(!RetVal)
        UNMANAGED_ERROR(13);

    // set and close event
    if(!SetEvent(InInfo->hRemoteSignal))
        UNMANAGED_ERROR(22);

    CloseHandle(InInfo->hRemoteSignal);

    InInfo->hRemoteSignal = NULL;

    // execute library code (no way for error reporting, so we dont need to check)
    RuntimeClrHost->ExecuteInDefaultAppDomain(
        InInfo->UserLibrary,
        L"EasyHook.InjectionLoader",
        L"Main",
        ParamString,
        &RetVal);

ABORT_ERROR:

    // release resources
    if(MetaClrHost != NULL)
        MetaClrHost->Release();
    if(RuntimeInfo  != NULL)
        RuntimeInfo->Release();
    if(RuntimeClrHost != NULL)
        RuntimeClrHost->Release();

    if(hMsCorEE != NULL)
        FreeLibrary(hMsCorEE);

    return ErrorCode;
}


EASYHOOK_NT_EXPORT HookCompleteInjection(LPREMOTE_INFO InInfo)
{
/*
Description:

    This method is directly called from assembler code. It will update
    the symbols with the ones of the current process, adjust the PATH
    variable and invoke one of two above injection completions.

*/

    WCHAR*	    	PATH = NULL;
	ULONG		    ErrorCode = 0;
	HMODULE         hMod = GetModuleHandleA("kernel32.dll");
    ULONG           DirSize;
    ULONG           EnvSize;

    /*
		To increase stability we will now update all symbols with the
		real local ones...
	*/
	InInfo->LoadLibraryW = GetProcAddress(hMod, "LoadLibraryW");
	InInfo->FreeLibrary = GetProcAddress(hMod, "FreeLibrary");
	InInfo->GetProcAddress = GetProcAddress(hMod, "GetProcAddress");
	InInfo->VirtualFree = GetProcAddress(hMod, "VirtualFree");
	InInfo->VirtualProtect = GetProcAddress(hMod, "VirtualProtect");
	InInfo->ExitThread = GetProcAddress(hMod, "ExitThread");
	InInfo->GetLastError = GetProcAddress(hMod, "GetLastError");

    

	/* 
        Make directory of user library path available to current process...
	    This is to find dependencies without copying them into a global
	    directory which might cause trouble.
    */

	DirSize = RtlUnicodeLength(InInfo->PATH);
	EnvSize = GetEnvironmentVariableW(L"PATH", NULL, 0) + DirSize;

	if((PATH = (wchar_t*)RtlAllocateMemory(TRUE, EnvSize * 2 + 10)) == NULL)
		UNMANAGED_ERROR(1);

	GetEnvironmentVariableW(L"PATH", PATH, EnvSize);

	// add library path to environment variable
	if(!RtlMoveMemory(PATH + DirSize, PATH, (EnvSize - DirSize) * 2))
        UNMANAGED_ERROR(1);

	RtlCopyMemory(PATH, InInfo->PATH, DirSize * 2);

	if(!SetEnvironmentVariableW(L"PATH", PATH))
		UNMANAGED_ERROR(2);

    if(!RTL_SUCCESS(RhSetWakeUpThreadID(InInfo->WakeUpThreadID)))
        UNMANAGED_ERROR(3);

	// load and execute user library...
	if(InInfo->IsManaged)
        ErrorCode = CompleteManagedInjection(InInfo);
    else
        ErrorCode = CompleteUnmanagedInjection(InInfo);

ABORT_ERROR:

    // release resources
	if(PATH != NULL)
		RtlFreeMemory(PATH);

	if(InInfo->hRemoteSignal != NULL)
		CloseHandle(InInfo->hRemoteSignal);

	return ErrorCode;
}
