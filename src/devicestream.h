//-----------------------------------------------------------------------------
// Copyright (c) 2016-2020 Michael G. Brehm
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

#ifndef __DEVICESTREAM_H_
#define __DEVICESTREAM_H_
#pragma once

#pragma warning(push, 4)

#include <memory>
#include <string>
#include <vector>

#include "pvrstream.h"

//---------------------------------------------------------------------------
// Class devicestream
//
// Implements an RTP/UDP based device stream

class devicestream : public pvrstream
{
public:

	// Destructor
	//
	virtual ~devicestream();

	//-----------------------------------------------------------------------
	// Member Functions

	// canseek
	//
	// Flag indicating if the stream allows seek operations
	bool canseek(void) const;

	// chunksize
	//
	// Gets the stream chunk size
	size_t chunksize(void) const;

	// close
	//
	// Closes the stream
	void close(void);

	// create (static)
	//
	// Factory method, creates a new devicestream instance
	static std::unique_ptr<devicestream> create(std::vector<std::string> const& devices, char const* vchannel);
	static std::unique_ptr<devicestream> create(std::vector<std::string> const& devices, char const* vchannel, size_t chunksize);

	// length
	//
	// Gets the length of the stream
	long long length(void) const;

	// mediatype
	//
	// Gets the media type of the stream
	char const* mediatype(void) const;

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

	devicestream(devicestream const&) = delete;
	devicestream& operator=(devicestream const&) = delete;

	// DEFAULT_CHUNK_SIZE
	//
	// Default stream chunk size
	static size_t const DEFAULT_CHUNK_SIZE;

	// DEFAULT_MEDIA_TYPE
	//
	// Default media type to report for the stream
	static char const* DEFAULT_MEDIA_TYPE;

	// MAXIMUM_WAIT_TIME
	//
	// The maximum amount of time to wait for stream data
	static uint64_t const MAXIMUM_WAIT_TIME;

	// WAIT_INTERVAL
	//
	// The amount of time to wait at once for stream data
	static uint64_t const WAIT_INTERVAL;

	// Instance Constructor
	//
	devicestream(struct hdhomerun_device_selector_t* selector, struct hdhomerun_device_t* device, size_t chunksize);

	//-----------------------------------------------------------------------
	// Member Variables

	struct hdhomerun_device_selector_t*		m_selector;			// Device selector
	struct hdhomerun_device_t*				m_device;			// Selected device
	size_t const							m_chunksize;		// Stream chunk size
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __DEVICESTREAM_H_
