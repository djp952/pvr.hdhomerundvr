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

#ifndef __DVRSTREAM_H_
#define __DVRSTREAM_H_
#pragma once

#pragma warning(push, 4)

#include <functional>
#include <memory>
#include <set>

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

	// currenttime
	//
	// Gets the current time of the stream
	time_t currenttime(void) const;

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

	// starttime
	//
	// Gets the starting time for the stream
	time_t starttime(void) const;

private:

	dvrstream(dvrstream const&)=delete;
	dvrstream& operator=(dvrstream const&)=delete;

	// DEFAULT_READ_MIN
	//
	// Default minimum amount of data to return from a read request
	static size_t const DEFAULT_READ_MINCOUNT;

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
	dvrstream(char const* url, size_t buffersize, size_t readmincount);

	//-----------------------------------------------------------------------
	// Private Member Functions

	// curl_responseheaders (static)
	//
	// libcurl callback to handle processing of response headers
	static size_t curl_responseheaders(char const* data, size_t size, size_t count, void* context);

	// curl_write (static)
	//
	// libcurl callback to write received data into the buffer
	static size_t curl_write(void const* data, size_t size, size_t count, void* context);

	// filter_packets
	//
	// Implements the transport stream packet filter
	void filter_packets(uint8_t* buffer, size_t count);

	// restart
	//
	// Restarts the stream at the specified position
	long long restart(long long position);

	// transfer_until
	//
	// Executes the data tranfer until the predicate has been satisfied
	bool transfer_until(std::function<bool(void)> predicate);

	//-----------------------------------------------------------------------
	// Member Variables

	// DATA TRANSFER
	//
	CURL*							m_curl = nullptr;				// CURL easy interface handle
	CURLM*							m_curlm = nullptr;				// CURL multi interface handle
	size_t const					m_readmincount;					// Minimum read byte count

	// STREAM STATE
	//
	bool							m_paused = false;				// Flag if transfer is paused
	bool							m_headers = false;				// Flag if headers have been processed
	bool							m_canseek = false;				// Flag if stream can be seeked
	long long						m_startpos = 0;					// Starting position
	long long						m_readpos = 0;					// Current read position
	long long						m_writepos = 0;					// Current write position
	long long						m_length = MAX_STREAM_LENGTH;	// Length of the stream
	time_t							m_starttime = 0;				// Start time of the stream
	uint64_t						m_startpts = 0;					// Starting presentation timestamp
	uint64_t						m_currentpts = 0;				// Current presentation timestamp

	// RING BUFFER
	//
	size_t const					m_buffersize;					// Size of the ring buffer
	std::unique_ptr<uint8_t[]>		m_buffer;						// Ring buffer stroage
	size_t							m_head = 0;						// Head (write) buffer position
	size_t							m_tail = 0;						// Tail (read) buffer position

	// PACKET FILTER
	//
	bool							m_enablefilter = true;			// Flag if packet filter is enabled
	std::set<uint16_t>				m_pmtpids;						// Set of PMT program ids
	bool							m_enablepcrs = true;			// Flag if PCR reads are enabled
	uint16_t						m_pcrpid = 0;					// Program Clock PID
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __DVRSTREAM_H_`
