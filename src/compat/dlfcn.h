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

#ifndef __DLFCN_H_
#define __DLFCN_H_
#pragma once

#ifndef _WINDOWS
#error this dlfcn.h is intended for use on Windows only
#endif

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// CONSTANTS / MACROS
//---------------------------------------------------------------------------

#define RTLD_LAZY			0x1
#define RTLD_NOW			0x2
#define RTLD_LOCAL			0x4
#define RTLD_GLOBAL			0x8
#define RTLD_NOLOAD			0x10
#define RTLD_SHARED			0x20
#define RTLD_UNSHARED		0x40
#define RTLD_NODELETE		0x80
#define RTLD_LAZY_UNDEF		0x100

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

// dlclose
//
// Closes a dynamic shared object handle obtained with dlopen
int dlclose(void* handle);

// dlerror
//
// Gets a description of the last error that occurred during dynamic linking
char* dlerror(void);

// dlopen
//
// Opens a dynamic shared object
void* dlopen(char const* filename, int flags);

// dlsym
//
// Obtains the address of a symbol in a shared object or executable
void* dlsym(void* handle, char const* symbol);

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __DLFCN_H_
