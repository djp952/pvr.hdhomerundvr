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

#ifndef __SCALAR_CONDITION_H_
#define __SCALAR_CONDITION_H_
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

#pragma warning(push, 4)				

//-----------------------------------------------------------------------------
// scalar_condition
//
// Implements a simple scalar value condition variable

template <typename _type>
class scalar_condition
{
public:

	// Instance Constructor
	//
	scalar_condition(_type initial) : m_value(initial) {}

	// Destructor
	//
	~scalar_condition()=default;

	// assignment operator
	//
	const scalar_condition& operator=(_type const& value) 
	{
		std::unique_lock<std::mutex> critsec(m_lock);

		// Change the stored value and wake up any threads waiting for it,
		// the predicate in WaitUntil handles spurious or no-change wakes
		m_value = value;
		m_condition.notify_all();

		return *this;
	}

	//-------------------------------------------------------------------------
	// Member Functions

	// test
	//
	// Tests the condition by executing a zero-millisecond wait
	bool test(_type const& value) const
	{
		return wait_until_equals(value, 0);
	}

	// wait_until_equals
	//
	// Waits indefinitely until the value has been set to the specified value
	void wait_until_equals(_type const& value) const
	{
		std::unique_lock<std::mutex> critsec(m_lock);

		// If the value does not already match the provided value, wait for it
		m_condition.wait(critsec, [&]() -> bool { return m_value == value; });
	}

	// wait_until_equals
	//
	// Waits until the value has been set to the specified value
	bool wait_until_equals(_type const& value, uint32_t timeoutms) const
	{
		std::unique_lock<std::mutex> critsec(m_lock);

		// If the value does not already match the provided value, wait for it
		return m_condition.wait_until(critsec, std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutms), 
			[&]() -> bool { return m_value == value; });
	}

private:

	scalar_condition(scalar_condition const&)=delete;
	scalar_condition& operator=(scalar_condition const& rhs)=delete;

	//-------------------------------------------------------------------------
	// Member Variables

	mutable std::condition_variable		m_condition;	// Condition variable
	mutable std::mutex					m_lock;			// Synchronization object
	_type								m_value;		// Contained value
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __SCALAR_CONDITION_H_
