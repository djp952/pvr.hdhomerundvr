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

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

#pragma warning(push, 4)				// Enable maximum compiler warnings

//-----------------------------------------------------------------------------
// cv_wait_until_equals
//
// Invokes condition_variable::wait_until() without _USE_32BIT_TIME_T defined to work
// around a problem with the VC2013 implementation where time_t has to be 64-bit
//
// Arguments:
//
//	cv			- Condition variable to be waited on
//	lock		- unique_lock<> in which to execute the wait
//	timeoutms	- Number of milliseconds to wait for the condition to signal
//	predicate	- Predicate function to pass into condition_variable::wait_until

bool cv_wait_until_equals(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, uint32_t timeoutms, std::function<bool(void)> predicate)
{
	return cv.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutms), predicate);
}

//---------------------------------------------------------------------------

#pragma warning(pop)
