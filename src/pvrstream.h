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

#ifndef __PVRSTREAM_H_
#define __PVRSTREAM_H_
#pragma once

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// Class pvrstream
//
// Defines the interface required for creating and manipulating PVR streams

class pvrstream
{
public:

	// Constructor / Destructor
	//
	pvrstream() {}
	virtual ~pvrstream() {}

	//-----------------------------------------------------------------------
	// Member Functions

	// canseek
	//
	// Flag indicating if the stream allows seek operations
	virtual bool canseek(void) const = 0;

	// close
	//
	// Closes the stream
	virtual void close(void) = 0;

	// currentpts
	//
	// Gets the current presentation timestamp
	virtual uint64_t currentpts(void) const = 0;
	
	// currenttime
	//
	// Gets the current time of the stream
	virtual time_t currenttime(void) const = 0;

	// length
	//
	// Gets the length of the stream
	virtual long long length(void) const = 0;

	// mediatype
	//
	// Gets the media type of the stream
	virtual char const* mediatype(void) const = 0;

	// position
	//
	// Gets the current position of the stream
	virtual long long position(void) const = 0;

	// read
	//
	// Reads available data from the stream
	virtual size_t read(uint8_t* buffer, size_t count) = 0;

	// realtime
	//
	// Gets a flag indicating if the stream is real-time
	virtual bool realtime(void) const = 0;

	// seek
	//
	// Sets the stream pointer to a specific position
	virtual long long seek(long long position, int whence) = 0;

	// startpts
	//
	// Gets the starting presentation timestamp
	virtual uint64_t startpts(void) const = 0;

	// starttime
	//
	// Gets the starting time for the stream
	virtual time_t starttime(void) const = 0;

private:

	pvrstream(pvrstream const&) = delete;
	pvrstream& operator=(pvrstream const&) = delete;
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __PVRSTREAM_H_
