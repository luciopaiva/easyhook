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

static PWCHAR           LastError = L"";
static ULONG            LastErrorCode = 0;

EASYHOOK_NT_EXPORT RtlGetLastError()
{
    return LastErrorCode;
}

PWCHAR RtlGetLastErrorString()
{
    return LastError;
}

PWCHAR RtlGetLastErrorStringCopy()
{
    // https://easyhook.codeplex.com/workitem/24958
    int len = (wcslen(LastError)+1)*sizeof(TCHAR);
    PWCHAR pBuffer = (PWCHAR) CoTaskMemAlloc(len);
    CopyMemory(pBuffer, LastError, len);

    return pBuffer;
}

void RtlSetLastError(LONG InCode, WCHAR* InMessage)
{
    LastErrorCode = InCode;

    if(InMessage == NULL)
        LastError = L"";
    else
        LastError = (PWCHAR)InMessage;
}

#ifndef DRIVER
	void RtlAssert(BOOL InAssert,LPCWSTR lpMessageText)
	{
		if(InAssert)
			return;

	#ifdef _DEBUG
		DebugBreak();
	#endif

			FatalAppExitW(0, lpMessageText);
		
	}
#endif