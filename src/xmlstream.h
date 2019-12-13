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

#ifndef __XMLSTREAM_H_
#define __XMLSTREAM_H_
#pragma once

#pragma warning(push, 4)

#include <functional>
#include <memory>

//---------------------------------------------------------------------------
// Class xmlstream
//
// Implements an HTTP-based XML stream reader

class xmlstream
{
public:

	// Destructor
	//
	~xmlstream();

	//-----------------------------------------------------------------------
	// Member Functions

	// close
	//
	// Closes the stream
	void close(void);

	// create (static)
	//
	// Factory method, creates a new httpstream instance
	static std::unique_ptr<xmlstream> create(char const* url);
	static std::unique_ptr<xmlstream> create(char const* url, char const* useragent);
	static std::unique_ptr<xmlstream> create(char const* url, char const* useragent, CURLSH* share);

	// read
	//
	// Reads available data from the stream
	size_t read(uint8_t* buffer, size_t count);

private:

	xmlstream(xmlstream const&)=delete;
	xmlstream& operator=(xmlstream const&)=delete;

	// DEFAULT_RINGBUFFER_SIZE
	//
	// Default ring buffer size, in bytes
	static size_t const DEFAULT_RINGBUFFER_SIZE;

	// Instance Constructor
	//
	xmlstream(char const* url, char const* useragent, CURLSH* share);

	//-----------------------------------------------------------------------
	// Private Member Functions

	// curl_write (static)
	//
	// libcurl callback to write received data into the buffer
	static size_t curl_write(void const* data, size_t size, size_t count, void* context);

	// transfer_until
	//
	// Executes the data tranfer until the predicate has been satisfied
	bool transfer_until(std::function<bool(void)> predicate);

	//-----------------------------------------------------------------------
	// Member Variables

	// DATA TRANSFER
	//
	CURL*						m_curl = nullptr;					// CURL easy interface handle
	CURLM*						m_curlm = nullptr;					// CURL multi interface handle
	bool						m_paused = false;					// Flag if transfer is paused

	// RING BUFFER
	//
	size_t const				m_buffersize;						// Size of the ring buffer
	std::unique_ptr<uint8_t[]>	m_buffer;							// Ring buffer stroage
	size_t						m_head = 0;							// Head (write) buffer position
	size_t						m_tail = 0;							// Tail (read) buffer position
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __HTTPSTREAM_H_
