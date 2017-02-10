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

#ifndef __STRING_EXCEPTION_H_
#define __STRING_EXCEPTION_H_
#pragma once

#include <exception>
#include <sstream>
#include <string>

#pragma warning(push, 4)	

//-----------------------------------------------------------------------------
// Class string_exception
//
// std::exception used to contain a simple message string

class string_exception : public std::exception
{
public:

	// Instance Constructor
	//
	template<typename... _args>
	string_exception(_args&&... args) : m_what(format_message(std::forward<_args>(args)...)) {}

	// Copy Constructor
	//
	string_exception(string_exception const& rhs) : m_what(rhs.m_what) {}

	// char const* conversion operator
	//
	operator char const*() const
	{
		return m_what.c_str();
	}

	//-------------------------------------------------------------------------
	// Member Functions

	// what (std::exception)
	//
	// Gets a pointer to the exception message text
	virtual char const* what(void) const noexcept override
	{
		return m_what.c_str();
	}
		
private:

	//-----------------------------------------------------------------------
	// Private Member Functions

	// format_message
	//
	// Variadic string generator used by the constructor
	template<typename... _args>
	static std::string format_message(_args&&... args)
	{
		std::ostringstream stream;
		int unpack[] = {0, ( static_cast<void>(stream << args), 0 ) ... };
		(void)unpack;

		return stream.str();
	}

	//-------------------------------------------------------------------------
	// Member Variables

	std::string					m_what;			// SQLite error message
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __STRING_EXCEPTION_H_