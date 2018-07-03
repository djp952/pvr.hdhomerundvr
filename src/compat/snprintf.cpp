//-----------------------------------------------------------------------------
// Copyright (c) 2018 Michael G. Brehm
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

#include <stdio.h>
#include <stdarg.h>

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// snprintf
//
// Writes formatted data to a string
//
// Arguments:
//
//	buffer			- Storage location for the output
//	count			- Maximum number of characters to store
//	format			- Format-control string
//	...				- Optional arguments

int snprintf(char* buffer, size_t count, char const* format, ...)
{
	if((buffer == nullptr) && (count > 0)) return -1;
	if(format == nullptr) return -1;

	va_list args;
	va_start(args, format);

	// Use Microsoft-specific _vsnprintf() on Visual Studio 2013
	int result = _vsnprintf(buffer, count, format, args);

	va_end(args);
	
	// Ensure zero termination of the buffer
	if(count > 0) buffer[count - 1] = '\0';

	return result;
}

//-----------------------------------------------------------------------------

#pragma warning(pop)
