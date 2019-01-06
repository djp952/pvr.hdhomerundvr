//---------------------------------------------------------------------------
// Copyright (c) 2016-2019 Michael G. Brehm
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
//---------------------------------------------------------------------------

#ifndef __STDAFX_H_
#define __STDAFX_H_
#pragma once

//---------------------------------------------------------------------------
// Windows
//---------------------------------------------------------------------------

#if defined(_WINDOWS) || defined(WINAPI_FAMILY)
#include <winapifamily.h>

// Windows Desktop
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

#define WINVER			_WIN32_WINNT_WIN8
#define	_WIN32_WINNT	_WIN32_WINNT_WIN8
#define	_WIN32_IE		_WIN32_IE_IE80
#define NOMINMAX

#include <Windows.h>
#define TARGET_WINDOWS

// Windows App
#elif WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>
#include <objbase.h>
#define TARGET_WINDOWS_STORE

#endif

#endif	// defined(_WINDOWS) || defined(WINAPI_FAMILY)

#include <assert.h>
#include <stdint.h>

// KiB / MiB / GiB
//
#define KiB		*(1 << 10)
#define MiB		*(1 << 20)
#define GiB		*(1 << 30)

//---------------------------------------------------------------------------
// SQLite
//---------------------------------------------------------------------------

#ifdef TARGET_WINDOWS_STORE
#define SQLITE_OS_WINRT 1
#endif

#define SQLITE_THREADSAFE 2
#define SQLITE_ENABLE_JSON1	1
#define SQLITE_TEMP_STORE 3
#include <sqlite3.h>

//--------------------------------------------------------------------------
// libcurl
//---------------------------------------------------------------------------

#define CURL_STATICLIB
#include <curl/curl.h>				// Include CURL declarations

//---------------------------------------------------------------------------

#endif	// __STDAFX_H_
