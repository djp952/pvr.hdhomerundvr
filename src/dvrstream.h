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

#ifndef __DVRSTREAM_H_
#define __DVRSTREAM_H_
#pragma once

#pragma warning(push, 4)

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include "scalar_condition.h"

//---------------------------------------------------------------------------
// Class dvrstream
//
// Implements an HTTP-based DVR stream ring buffer

class dvrstream
{
public:

	// Destructor
	//
	~dvrstream();

	//-----------------------------------------------------------------------
	// Member Functions

	// canseek
	//
	// Flag indicating if the stream allows seek operations
	bool canseek(void) const;

	// close
	//
	// Closes the stream
	void close(void);

	// create (static)
	//
	// Factory method, creates a new dvrstream instance
	static std::unique_ptr<dvrstream> create(char const* url);
	static std::unique_ptr<dvrstream> create(char const* url, size_t buffersize);
	static std::unique_ptr<dvrstream> create(char const* url, size_t buffersize, size_t readmincount);
	static std::unique_ptr<dvrstream> create(char const* url, size_t buffersize, size_t readmincount, unsigned int readtimeout);

	// length
	//
	// Gets the length of the stream
	long long length(void) const;

	// position
	//
	// Gets the current position of the stream
	long long position(void) const;

	// read
	//
	// Reads available data from the stream
	size_t read(uint8_t* buffer, size_t count);

	// realtime
	//
	// Gets a flag indicating if the stream is real-time
	bool realtime(void) const;

	// seek
	//
	// Sets the stream pointer to a specific position
	long long seek(long long position, int whence);

private:

	dvrstream(dvrstream const&)=delete;
	dvrstream& operator=(dvrstream const&)=delete;

	// DEFAULT_READ_MIN
	//
	// Default minimum amount of data to return from a read request
	static size_t const DEFAULT_READ_MINCOUNT;

	// DEFAULT_READ_TIMEOUT_MS
	//
	// Default amount of time for a read operation to succeed
	static unsigned int const DEFAULT_READ_TIMEOUT_MS;

	// DEFAULT_RINGBUFFER_SIZE
	//
	// Default ring buffer size, in bytes
	static size_t const DEFAULT_RINGBUFFER_SIZE;

	// MAX_STREAM_LENGTH
	//
	// Maximum allowable stream length; indicates a real-time stream
	static long long const MAX_STREAM_LENGTH;

	// MPEGTS_PACKET_LENGTH
	//
	// Length of a single mpeg-ts data packet
	static size_t const MPEGTS_PACKET_LENGTH;

	// Instance Constructor
	//
	dvrstream(char const* url, size_t buffersize, size_t readmincount, unsigned int readtimeout);

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

	// curl_transfer_func
	//
	// Worker thread procedure for the CURL data transfer
	void curl_transfer_func(long long position);

	// curl_write (static)
	//
	// libcurl callback to write received data into the buffer
	static size_t curl_write(void const* data, size_t size, size_t count, void* context);

	// filter_packets
	//
	// Implements the transport stream packet filter
	void filter_packets(std::unique_lock<std::mutex> const& lock, uint8_t* buffer, size_t count);

	// restart
	//
	// Restarts the stream at the specified position
	long long restart(std::unique_lock<std::mutex>& lock, long long position);

	//-----------------------------------------------------------------------
	// Member Variables

	mutable std::mutex				m_lock;						// Synchronization object
	std::condition_variable			m_cv;						// Transfer event condvar
	std::mutex						m_writelock;				// Seek/write sync object

	// DATA TRANSFER
	//
	std::thread						m_worker;					// Data transfer thread
	CURL*							m_curl = nullptr;			// CURL transfer object
	CURLcode						m_curlresult = CURLE_OK;	// CURL transfer result
	char							m_curlerr[CURL_ERROR_SIZE];	// CURL error message

	// STREAM CONTROL
	//
	scalar_condition<bool>			m_started{false};			// Stream started condition
	std::atomic<bool>				m_stop{false};				// Flag to stop the transfer
	std::atomic<bool>				m_paused{false};			// Flag if transfer is paused
	std::atomic<bool>				m_stopped{false};			// Data transfer stopped flag

	// STREAM INFORMATION
	//
	size_t const					m_readmincount;				// Minimum read byte count
	unsigned int const				m_readtimeout;				// Read timeout in milliseconds
	bool							m_canseek = false;			// Flag if stream can be seeked
	long long						m_startpos = 0;				// Starting position
	long long						m_readpos = 0;				// Current read position
	long long						m_writepos = 0;				// Current write position
	long long						m_length = 0;				// Length of the stream

	// RING BUFFER
	//
	size_t const					m_buffersize;				// Size of the ring buffer
	std::unique_ptr<uint8_t[]>		m_buffer;					// Ring buffer stroage
	std::atomic<size_t>				m_bufferhead{0};			// Head (write) buffer position
	std::atomic<size_t>				m_buffertail{0};			// Tail (read) buffer position

	// PACKET FILTER
	//
	std::set<uint16_t>				m_pmtpids;					// Set of PMT program ids
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __DVRSTREAM_H_`
