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

#ifndef __HTTP_EXCEPTION_H_
#define __HTTP_EXCEPTION_H_
#pragma once

#include <HttpStatusCodes_C++11.h>

#include "string_exception.h"

#pragma warning(push, 4)	

//-----------------------------------------------------------------------------
// Class http_exception
//
// Specialization of string_exception for HTTP response codes

class http_exception : public string_exception
{
public:

	// Instance Constructor
	//
	http_exception(long responsecode) : string_exception("HTTP ", responsecode, ": ", HttpStatus::reasonPhrase(responsecode)), m_responsecode(responsecode) {}

	//-------------------------------------------------------------------------
	// Member Functions

	// responsecode
	//
	// Gets the HTTP response code associated with the exception
	long responsecode(void) const noexcept
	{
		return m_responsecode;
	}
		
private:

	//-------------------------------------------------------------------------
	// Member Variables

	long 				m_responsecode;			// HTTP response code
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __HTTP_EXCEPTION_H_