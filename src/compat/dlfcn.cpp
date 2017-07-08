//-----------------------------------------------------------------------------
// Copyright (c) 2017 Michael G. Brehm
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//-----------------------------------------------------------------------------

#include "stdafx.h"
#include "dlfcn.h"

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// dlclose
//
// Closes a dynamic shared object handle obtained with dlopen
//
// Arguments:
//
//	handle		- Handle returned from successful call to dlopen

int dlclose(void* handle)
{
	if(handle) FreeLibrary(reinterpret_cast<HMODULE>(handle));
	return (handle) ? 0 : -1;
}

//---------------------------------------------------------------------------
// dlerror
//
// Gets a description of the last error that occurred during dynamic linking
//
// Arguments:
//
//	NONE

char* dlerror(void)
{
	// Not implemented
	return nullptr;
}

//---------------------------------------------------------------------------
// dlopen
//
// Opens a dynamic shared object
//
// Arguments:
//
//	filename	- Name of the shared object file to open
//	flags		- Operational flags (ignored)

void* dlopen(char const* filename, int /*flags*/)
{
	// Ignore the specified flags; just call LoadLibraryEx on Windows
	return reinterpret_cast<void*>(LoadLibraryExA(filename, nullptr, 0));
}

//---------------------------------------------------------------------------
// dlsym
//
// Obtains the address of a symbol in a shared object or executable
//
// Arguments:
//
//	handle		- Handled returned from successful call to dlopen
//	symbol		- Name of the symbol to local in the shared object

void* dlsym(void* handle, char const* symbol)
{
	return GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol);
}

//-----------------------------------------------------------------------------

#pragma warning(pop)
