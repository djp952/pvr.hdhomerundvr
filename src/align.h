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

#ifndef __ALIGN_H_
#define __ALIGN_H_
#pragma once

#include <stdexcept>
#include <type_traits>
#include <stdint.h>

#pragma warning(push, 4)

// __align
//
// Namespace declarations private to align::up and align::down to prevent name clashes
namespace __align {

	// __align::is_signed_integral
	//
	// Trait to determine if a type is a signed integal
	template <typename value_type> struct is_signed_integral : 
	public std::integral_constant<bool, !std::is_floating_point<value_type>::value && std::is_signed<value_type>::value> {};

	// __align::is_unsigned_integral
	//
	// Trait to determine if a type is an unsigned integral
	template <typename value_type> struct is_unsigned_integral : 
	public std::integral_constant<bool, std::is_integral<value_type>::value && std::is_unsigned<value_type>::value> {};
};

// align::up
// align::down
//
// Aligns a pointer or an integral value up or down to an alignment boundary
namespace align {

	// align::up (pointers)
	//
	template <typename _type>
	inline typename std::enable_if<std::is_pointer<_type>::value, _type>::type up(_type value, unsigned int alignment)
	{
		if(alignment < 1) throw std::out_of_range("alignment");
		uintptr_t address = uintptr_t(value);
		return reinterpret_cast<_type>((address == 0) ? 0 : address + ((alignment - (address % alignment)) % alignment));
	}

	// align::up (unsigned integers)
	//
	template <typename _type>
	inline typename std::enable_if<__align::is_unsigned_integral<_type>::value, _type>::type up(_type value, unsigned int alignment)
	{
		if(alignment < 1) throw std::out_of_range("alignment");
		return static_cast<_type>((value == 0) ? 0 : value + ((alignment - (value % alignment)) % alignment));
	}

	// align::up (signed integers)
	//
	template <typename _type>
	inline typename std::enable_if<__align::is_signed_integral<_type>::value, _type>::type up(_type value, int alignment)
	{
		if(alignment < 1) throw std::out_of_range("alignment");
		return static_cast<_type>((value == 0) ? 0 : value + ((alignment - (value % alignment)) % alignment));
	}

	// align::down (pointers)
	//
	template <typename _type>
	inline typename std::enable_if<std::is_pointer<_type>::value, _type>::type down(_type value, unsigned int alignment)
	{
		if(alignment < 1) throw std::out_of_range("alignment");
		uintptr_t address = uintptr_t(value);
		return reinterpret_cast<_type>((address < alignment) ? 0 : up<uintptr_t>(address - (alignment - 1), alignment));
	}

	// align::down (unsigned integers)
	//
	template <typename _type>
	inline typename std::enable_if<__align::is_unsigned_integral<_type>::value, _type>::type down(_type value, unsigned int alignment)
	{
		if(alignment < 1) throw std::out_of_range("alignment");
		return static_cast<_type>((value < alignment) ? 0 : up<_type>(value - (alignment - 1), alignment));
	}

	// align::down (signed integers)
	//
	template <typename _type>
	inline typename std::enable_if<__align::is_signed_integral<_type>::value, _type>::type down(_type value, int alignment)
	{
		if(alignment < 1) throw std::out_of_range("alignment");
		return static_cast<_type>((value < alignment) ? 0 : up<_type>(value - (alignment - 1), alignment));
	}
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __ALIGN_H_
