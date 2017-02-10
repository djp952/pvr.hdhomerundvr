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

#ifndef __LIVESTREAM_H_
#define __LIVESTREAM_H_
#pragma once

#pragma warning(push, 4)

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include "scalar_condition.h"

//---------------------------------------------------------------------------
// Class livestream
//
// Implements a live radio/tv http stream buffer

class livestream
{
public:

	// Instance Constructor
	//
	livestream(size_t buffersize);

	// Destructor
	//
	~livestream();

	//-----------------------------------------------------------------------
	// Member Functions

	// length
	//
	// Gets the length of the live stream as transferred thus far
	uint64_t length(void) const;

	// position
	//
	// Gets the current position of the live stream
	uint64_t position(void) const;

	// read
	//
	// Reads data from the live stream into a destination buffer
	size_t read(uint8_t* buffer, size_t count, uint32_t timeoutms);

	// seek
	//
	// Stops and restarts the transfer at a specific position
	uint64_t seek(uint64_t position);

	// start
	//
	// Begins the transfer into the live stream buffer
	uint64_t start(char const* url);

	// stop
	//
	// Stops the data transfer into the live stream buffer
	uint64_t stop(void);

private:

	livestream(livestream const&)=delete;
	livestream& operator=(livestream const&)=delete;

	//-----------------------------------------------------------------------
	// Private Member Functions

	// curl_responseheaders (static)
	//
	// libcurl callback to handle processing of response headers
	static size_t curl_responseheaders(char const* data, size_t size, size_t count, void* context);

	// curl_transfercontrol (static)
	//
	// libcurl callback to handle transfer information/progress
	static int curl_transfercontrol(void* context, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

	// curl_write (static)
	//
	// libcurl callback to write received data into the buffer
	static size_t curl_write(void const* data, size_t size, size_t count, void* context);

	// reset_stream_state
	//
	// Resets all of the stream state variables
	void reset_stream_state(std::unique_lock<std::mutex> const& lock);

	//-----------------------------------------------------------------------
	// Member Variables

	std::thread					m_worker;					// Data transfer thread
	mutable std::mutex			m_lock;						// Synchronization object
	CURL*						m_curl = nullptr;			// CURL transfer object

	// STREAM CONTROL
	//
	scalar_condition<bool>		m_started{false};			// Worker started condition
	std::atomic<bool>			m_stop{false};				// Flag to stop the transfer
	std::atomic<bool>			m_paused{false};			// Flag if transfer is paused

	// STREAM INFORMATION
	//
	uint64_t					m_readpos = 0;				// Current read position
	uint64_t					m_writepos = 0;				// Current write position
	std::atomic<uint64_t>		m_length{0};				// Known end of the stream

	// RING BUFFER
	//
	size_t const				m_buffersize;				// Size of the ring buffer
	std::unique_ptr<uint8_t[]>	m_buffer;					// Ring buffer stroage
	bool						m_bufferempty = true;		// Flag for empty ring buffer
	bool						m_bufferfull = false;		// Flag for full ring buffer
	std::atomic<size_t>			m_bufferhead{0};			// Head (write) buffer position
	std::atomic<size_t>			m_buffertail{0};			// Tail (read) buffer position
	std::condition_variable		m_bufferhasdata;			// Signals that data is available
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __LIVESTREAM_H_
