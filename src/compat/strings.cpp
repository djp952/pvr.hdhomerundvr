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
#include "strings.h"

#include <string.h>

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// strcasecmp
//
// Compares the two strings s1 and s2, ignoring the case of the characters
//
// Arguments:
//
//	s1		- Left-hand string to be compared
//	s2		- Right-hand string to be compared

int strcasecmp(char const* s1, char const* s2)
{
	return _stricmp(s1, s2);
}

//---------------------------------------------------------------------------
// strncasecmp
//
// Compares the two strings s1 and s2, ignoring the case of the characters
//
// Arguments:
//
//	s1		- Left-hand string to be compared
//	s2		- Right-hand string to be compared

int strncasecmp(char const* s1, char const* s2, size_t n)
{
	return _strnicmp(s1, s2, n);
}

//-----------------------------------------------------------------------------

#pragma warning(pop)
