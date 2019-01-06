//-----------------------------------------------------------------------------
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
//-----------------------------------------------------------------------------

#include "stdafx.h"
#include "uuid/uuid.h"

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// uuid_generate
//
// Generates a new UUID
//
// Arguments:
//
//	uuid		- Reference to uuid_t to receive the UUID

void uuid_generate(uuid_t& out)
{
	memset(&out, 0, sizeof(uuid_t));
	CoCreateGuid(&out);
}

//---------------------------------------------------------------------------
// uuid_unparse
//
// Converts a UUID into a string representation.  Caller must ensure that
// the provided output buffer is sufficient for 36+1 characters
//
// Arguments:
//
//	u		- UUID to be converted
//	out		- Output string buffer, MUST BE AT LEAST 36+1 BYTES LONG

void uuid_unparse(uuid_t const& u, char* out)
{
	if(out == nullptr) return;

	// UuidToStringA is only available on WINAPI_PARTITION_DESKTOP, this code is also
	// now used for the UWP/Store version of the library - just use sprintf()
	sprintf(out, "%08lx-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx", u.Data1, u.Data2, u.Data3, 
		u.Data4[0], u.Data4[1], u.Data4[2], u.Data4[3], u.Data4[4], u.Data4[5], u.Data4[6], u.Data4[7]);
}

//-----------------------------------------------------------------------------

#pragma warning(pop)
