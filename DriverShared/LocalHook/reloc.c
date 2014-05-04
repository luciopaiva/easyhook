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
#include "disassembler/udis86.h"

// GetInstructionLength_x64/x86 were replaced with the Udis86 library
// (http://udis86.sourceforge.net) see udis86.h/.c for appropriate
// licensing and copyright notices.

EASYHOOK_NT_INTERNAL LhGetInstructionLength(void* InPtr)
{
/*
Description:

    Takes a pointer to machine code and returns the length of the
    referenced instruction in bytes.
    
Returns:
    STATUS_INVALID_PARAMETER

        The given pointer references invalid machine code.
*/
	LONG			length = -1;
	// some exotic instructions might not be supported see the project
    // at https://github.com/vmt/udis86 and the forums.

    ud_t ud_obj;
    ud_init(&ud_obj);
#ifdef _M_X64
    ud_set_mode(&ud_obj, 64);
#else
    ud_set_mode(&ud_obj, 32);
#endif
    ud_set_input_buffer(&ud_obj, (uint8_t *)InPtr, 32);
    length = ud_disassemble(&ud_obj); // usually only between 1 and 5

	if(length > 0)
		return length;
	else
		return STATUS_INVALID_PARAMETER;
}

EASYHOOK_NT_INTERNAL LhRoundToNextInstruction(
			void* InCodePtr,
			ULONG InCodeSize)
{
/*
Description:

    Will round the given code size up so that the return
    value spans at least over "InCodeSize" bytes and always
    ends on instruction boundaries.

Parameters:

    - InCodePtr

        A code portion the given size should be aligned to.

    - InCodeSize

        The minimum return value.

Returns:

    STATUS_INVALID_PARAMETER

        The given pointer references invalid machine code.
*/
	UCHAR*				Ptr = (UCHAR*)InCodePtr;
	UCHAR*				BasePtr = Ptr;
    NTSTATUS            NtStatus;

	while(BasePtr + InCodeSize > Ptr)
	{
		FORCE(NtStatus = LhGetInstructionLength(Ptr));

		Ptr += NtStatus;
	}

	return (ULONG)(Ptr - BasePtr);

THROW_OUTRO:
    return NtStatus;
}

EASYHOOK_NT_INTERNAL LhDisassembleInstruction(void* InPtr, ULONG* length, PSTR buf, LONG buffSize, ULONG64 *nextInstr)
{
/*
Description:

    Takes a pointer to machine code and returns the length and
    ASM code for the referenced instruction.
    
Returns:
    STATUS_INVALID_PARAMETER

        The given pointer references invalid machine code.
*/
    // some exotic instructions might not be supported see the project
    // at https://github.com/vmt/udis86.

    ud_t ud_obj;
    ud_init(&ud_obj);
#ifdef _M_X64
    ud_set_mode(&ud_obj, 64);
#else
    ud_set_mode(&ud_obj, 32);
#endif
    ud_set_syntax(&ud_obj, UD_SYN_INTEL);
    ud_set_asm_buffer(&ud_obj, buf, buffSize);
    ud_set_input_buffer(&ud_obj, (uint8_t *)InPtr, 32);
    *length = ud_disassemble(&ud_obj);
    
    *nextInstr = (ULONG64)InPtr + *length;

    if(length > 0)
        return STATUS_SUCCESS;
    else
        return STATUS_INVALID_PARAMETER;
}

EASYHOOK_NT_INTERNAL LhRelocateRIPRelativeInstruction(
            ULONGLONG InOffset,
            ULONGLONG InTargetOffset,
            BOOL* OutWasRelocated)
{
/*
Description:

    Check whether the given instruction is RIP relative and
    relocates it. If it is not RIP relative, nothing is done.
    Only applicable to 64-bit processes, 32-bit will always
    return FALSE.

Parameters:

    - InOffset

        The instruction pointer to check for RIP addressing and relocate.

    - InTargetOffset

        The instruction pointer where the RIP relocation should go to.
        Please note that RIP relocation are relocated relative to the
        offset you specify here and therefore are still not absolute!

    - OutWasRelocated

        TRUE if the instruction was RIP relative and has been relocated,
        FALSE otherwise.
*/

#ifndef _M_X64
    return FALSE;
#else
    NTSTATUS            NtStatus;
    CHAR                    Buf[MAX_PATH];
    ULONG                    AsmSize;
    ULONG64                    NextInstr;
    CHAR                    Line[MAX_PATH];
    LONG                    Pos;
    LONGLONG                RelAddr;
    LONGLONG                MemDelta = InTargetOffset - InOffset;

    ASSERT(MemDelta == (LONG)MemDelta,L"reloc.c - MemDelta == (LONG)MemDelta");

    *OutWasRelocated = FALSE;

    // test field...
    /*BYTE t[10] = {0x8b, 0x05, 0x12, 0x34, 0x56, 0x78};
    // udis86 outputs: 0000000000000000 8b0512345678     mov eax, [rip+0x78563412]

    InOffset = (LONGLONG)t;

    MemDelta = InTargetOffset - InOffset;
*/

    // Disassemble the current instruction
    if(!RTL_SUCCESS(LhDisassembleInstruction((void*)InOffset, &AsmSize, Buf, sizeof(Buf), &NextInstr)))
        THROW(STATUS_INVALID_PARAMETER_1, L"Unable to disassemble entry point. ");
    
    // Check that the address is RIP relative (i.e. look for "[rip+")
    Pos = RtlAnsiIndexOf(Buf, '[');
      if(Pos < 0)
        RETURN;

    if (Buf[Pos + 1] == 'r' && Buf[Pos + 2] == 'i' && Buf[Pos + 3] == 'p' &&  Buf[Pos + 4] == '+')
    {
        Pos += 4;
        // parse content
        if(RtlAnsiSubString(Buf, Pos + 1, RtlAnsiIndexOf(Buf, ']') - Pos - 1, Line, MAX_PATH) <= 0)
            RETURN;

        // Convert HEX string to LONG
        RelAddr = strtol(Line, NULL, 16);
        if (!RelAddr)
            RETURN;

        // Verify that we are really RIP relative...
        if(RelAddr != (LONG)RelAddr)
            RETURN;
        // Ensure the RelAddr is equal to the RIP address in code
        if(*((LONG*)(NextInstr - 4)) != RelAddr)
            RETURN;
    
        /*
            Relocate this instruction...
        */
        // Adjust the relative address
        RelAddr = RelAddr - MemDelta;
        // Ensure the RIP address can still be relocated
        if(RelAddr != (LONG)RelAddr)
            THROW(STATUS_NOT_SUPPORTED, L"The given entry point contains at least one RIP-Relative instruction that could not be relocated!");

        // Copy instruction to target
        RtlCopyMemory((void*)InTargetOffset, (void*)InOffset, (ULONG)(NextInstr - InOffset));
        // Correct the rip address
        *((LONG*)(InTargetOffset + (NextInstr - InOffset) - 4)) = (LONG)RelAddr;

        *OutWasRelocated = TRUE;
    }

    RETURN;

THROW_OUTRO:
FINALLY_OUTRO:
    return NtStatus;
#endif
}

EASYHOOK_NT_INTERNAL LhRelocateEntryPoint(
				UCHAR* InEntryPoint,
				ULONG InEPSize,
				UCHAR* Buffer,
				ULONG* OutRelocSize)
{
/*
Description:

    Relocates the given entry point into the buffer and finally
    stores the relocated size in OutRelocSize.

Parameters:

    - InEntryPoint

        The entry point to relocate.

    - InEPSize

        Size of the given entry point in bytes.

    - Buffer

        A buffer receiving the relocated entry point.
        To ensure that there is always enough space, you should
        reserve around 100 bytes. After completion this method will
        store the real size in bytes in "OutRelocSize".
		Important: all instructions using RIP relative addresses will 
		be relative to the buffer location in memory.

    - OutRelocSize

        Receives the size of the relocated entry point in bytes.

Returns:

*/
#ifdef _M_X64
    #define POINTER_TYPE    LONGLONG
#else
    #define POINTER_TYPE    LONG
#endif
	UCHAR*				pRes = Buffer;
	UCHAR*				pOld = InEntryPoint;
    UCHAR			    b1;
	UCHAR			    b2;
	ULONG			    OpcodeLen;
	POINTER_TYPE   	    AbsAddr;
	BOOL			    a16;
	BOOL			    IsRIPRelative;
    ULONG               InstrLen;
    NTSTATUS            NtStatus;

	ASSERT(InEPSize < 20,L"reloc.c - InEPSize < 20");

	while(pOld < InEntryPoint + InEPSize)
	{
		b1 = *(pOld);
		b2 = *(pOld + 1);
		OpcodeLen = 0;
		AbsAddr = 0;
		a16 = FALSE;
		IsRIPRelative = FALSE;

		// check for prefixes
		switch(b1)
		{
		case 0x67: a16 = TRUE; continue;
		}

		/////////////////////////////////////////////////////////
		// get relative address value
		switch(b1)
		{
			case 0xE9: // jmp imm16/imm32
			{
				/* only allowed as first instruction and only if the trampoline can be planted 
				   within a 32-bit boundary around the original entrypoint. So the jumper will 
				   be only 5 bytes and whereever the underlying code returns it will always
				   be in a solid state. But this can only be guaranteed if the jump is the first
				   instruction... */
				if(pOld != InEntryPoint)
					THROW(STATUS_NOT_SUPPORTED, L"Hooking far jumps is only supported if they are the first instruction.");
				
				// ATTENTION: will continue in "case 0xE8"
			}
		case 0xE8: // call imm16/imm32
			{
				if(a16)
				{
					AbsAddr = *((__int16*)(pOld + 1));
					OpcodeLen = 3;
				}
				else
				{
					AbsAddr = *((__int32*)(pOld + 1));
					OpcodeLen = 5;
				}
			}break;

        case 0xEB: // jmp imm8
            {
                AbsAddr = *((__int8*)(pOld + 1));
                OpcodeLen = 2;
            }break;
        /*
			The problem with (conditional) jumps is that there will be no return into the relocated entry point.
			So the execution will be proceeded in the original method and this will cause the whole
			application to remain in an unstable state. Only near jumps with 32-bit offset are allowed as
			first instruction (see above)...
		*/
		case 0xE3: // jcxz imm8
			{
				THROW(STATUS_NOT_SUPPORTED, L"Hooking near (conditional) jumps is not supported.");
			}break;
		case 0x0F:
			{
				if((b2 & 0xF0) == 0x80) // jcc imm16/imm32
					THROW(STATUS_NOT_SUPPORTED,  L"Hooking far conditional jumps is not supported.");
			}break;
		}

		if((b1 & 0xF0) == 0x70) // jcc imm8
			THROW(STATUS_NOT_SUPPORTED,  L"Hooking near conditional jumps is not supported.");

		/////////////////////////////////////////////////////////
		// convert to: mov eax, AbsAddr

		if(OpcodeLen > 0)
		{
			AbsAddr += (POINTER_TYPE)(pOld + OpcodeLen);

#ifdef _M_X64
			*(pRes++) = 0x48; // REX.W-Prefix
#endif
			*(pRes++) = 0xB8;

			*((LONGLONG*)pRes) = AbsAddr;

			pRes += sizeof(void*);

			// points into entry point?
			if((AbsAddr >= (LONGLONG)InEntryPoint) && (AbsAddr < (LONGLONG)InEntryPoint + InEPSize))
				/* is not really unhookable but not worth the effort... */
				THROW(STATUS_NOT_SUPPORTED, L"Hooking jumps into the hooked entry point is not supported.");

			/////////////////////////////////////////////////////////
			// insert alternate code
			switch(b1)
			{
			case 0xE8: // call eax
				{
					*(pRes++) = 0xFF;
					*(pRes++) = 0xD0;
				}break;
			case 0xE9: // jmp eax
            case 0xEB: // jmp imm8
				{
					*(pRes++) = 0xFF;
					*(pRes++) = 0xE0;
				}break;
			}

			/* such conversions shouldnt be necessary in general...
			   maybe the method was already hooked or uses some hook protection or is just
			   bad programmed. EasyHook is capable of hooking the same method
			   many times simultanously. Even if other (unknown) hook libraries are hooking methods that
			   are already hooked by EasyHook. Only if EasyHook hooks methods that are already
			   hooked with other libraries there can be problems if the other libraries are not
			   capable of such a "bad" circumstance.
			*/

			*OutRelocSize = (ULONG)(pRes - Buffer);
		}
		else
		{
            // Check for RIP relative instructions and relocate
            LhRelocateRIPRelativeInstruction((ULONGLONG)pOld, (ULONGLONG)pRes, &IsRIPRelative);
		}

		// find next instruction
		FORCE(InstrLen = LhGetInstructionLength(pOld));

		if(OpcodeLen == 0)
		{
			// just copy the instruction
			if(!IsRIPRelative)
				RtlCopyMemory(pRes, pOld, InstrLen);

			pRes += InstrLen;
		}

		pOld += InstrLen;
		IsRIPRelative = FALSE;
	}

	*OutRelocSize = (ULONG)(pRes - Buffer);

	RETURN(STATUS_SUCCESS);

THROW_OUTRO:
FINALLY_OUTRO:
    return NtStatus;
}