//-----------------------------------------------------------------------------
// Copyright (c) 2016-2022 Michael G. Brehm
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

#ifndef __RADIOFILTER_H_
#define __RADIOFILTER_H_
#pragma once

#pragma warning(push, 4)

#include <memory>
#include <set>

#include "pvrstream.h"

//---------------------------------------------------------------------------
// Class radiofilter
//
// Implements a pvrstream wrapper that filters out any video elementary stream
// information from the MPEG-TS Program Map Table (PMT)

class radiofilter : public pvrstream
{
public:

	// Destructor
	//
	virtual ~radiofilter();

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
	// Factory method, creates a new radiofilter instance
	static std::unique_ptr<radiofilter> create(std::unique_ptr<pvrstream> basestream);

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

	radiofilter(radiofilter const&) = delete;
	radiofilter& operator=(radiofilter const&) = delete;

	// Instance Constructor
	//
	radiofilter(std::unique_ptr<pvrstream> basestream);

	// MPEGTS_PACKET_LENGTH
	//
	// Length of a single mpeg-ts data packet
	static size_t const MPEGTS_PACKET_LENGTH;

	//-----------------------------------------------------------------------
	// Private Member Functions

	// filter_packets
	//
	// Implements the transport stream packet filter
	void filter_packets(uint8_t* buffer, size_t count);

	//-----------------------------------------------------------------------
	// Member Variables

	std::unique_ptr<pvrstream> const	m_basestream;			// Underlying stream instance
	bool								m_enablefilter = true;	// Flag if packet filter is enabled
	std::set<uint16_t>					m_pmtpids;				// Set of PMT program ids
	std::set<uint16_t>					m_videopids;			// Set of video stream program ids
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __RADIOFILTER_H_
