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

#ifndef __CRYPTO_H_
#define __CRYPTO_H_
#pragma once

#ifndef _WINDOWS
#error this crypto.h is intended for use on Windows only
#endif

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// CONSTANTS / MACROS
//---------------------------------------------------------------------------

#define CRYPTO_LOCK			1
#define CRYPTO_UNLOCK		2
#define CRYPTO_READ			4
#define CRYPTO_WRITE		8

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

// CRYPTO_num_locks
//
// Gets the number of required synchronization objects
int CRYPTO_num_locks(void);

// CRYPTO_set_id_callback
//
// Sets a callback function that returns the current thread identifier
void CRYPTO_set_id_callback(unsigned long(*func)(void));

// CRYPTO_set_locking_callback
//
// Sets a callback used to lock or unlock a synchronization object
void CRYPTO_set_locking_callback(void(*func)(int mode, int n, char const* file, int line));

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __UUID_H_
