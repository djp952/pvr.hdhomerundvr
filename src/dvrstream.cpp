//---------------------------------------------------------------------------
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
//---------------------------------------------------------------------------

#include "stdafx.h"
#include "dvrstream.h"

#include <algorithm>
#include <assert.h>
#include <string.h>

#include "align.h"
#include "http_exception.h"
#include "string_exception.h"

#pragma warning(push, 4)

// dvrstream::DEFAULT_READ_MINCOUNT (static)
//
// Default minimum amount of data to return from a read request
size_t const dvrstream::DEFAULT_READ_MINCOUNT = (4 KiB);

// dvrstream::DEFAULT_RINGBUFFER_SIZE (static)
//
// Default ring buffer size, in bytes
size_t const dvrstream::DEFAULT_RINGBUFFER_SIZE = (1 MiB);

// dvrstream::MAX_STREAM_LENGTH (static)
//
// Maximum allowable stream length; indicates a real-time stream
long long const dvrstream::MAX_STREAM_LENGTH = std::numeric_limits<long long>::max();

// dvrstream::MPEGTS_PACKET_LENGTH (static)
//
// Length of a single mpeg-ts data packet
size_t const dvrstream::MPEGTS_PACKET_LENGTH = 188;
	
//---------------------------------------------------------------------------
// decode_pcr90khz
//
// Decodes a PCR (Program Clock Reference) value at the 90KHz clock
//
// Arguments:
//
//	ptr		- Pointer to the data to be decoded

inline uint64_t decode_pcr90khz(uint8_t const* ptr)
{
	assert(ptr != nullptr);

	// The 90KHz clock is encoded as a single 33 bit value at the start of the data
	return (static_cast<uint64_t>(ptr[0]) << 25) | (static_cast<uint32_t>(ptr[1]) << 17) | (static_cast<uint32_t>(ptr[2]) << 9) | (static_cast<uint16_t>(ptr[3]) << 1) | (ptr[4] >> 7);
}

//---------------------------------------------------------------------------
// decode_pcr27mhz
//
// Decodes a PCR (Program Clock Reference) value at the 27MHz clock
//
// Arguments:
//
//	ptr		- Pointer to the data to be decoded

inline uint64_t decode_pcr27mhz(uint8_t const* ptr)
{
	assert(ptr != nullptr);

	// The 27Mhz clock is decoded by multiplying the 33-bit 90KHz base clock by 300 and adding the 9-bit extension
	return (decode_pcr90khz(ptr) * 300) + (static_cast<uint16_t>(ptr[4] & 0x01) << 8) + ptr[5];
}

//---------------------------------------------------------------------------
// read_be8
//
// Reads a big-endian 8 bit value from memory
//
// Arguments:
//
//	ptr		- Pointer to the data to be read

inline uint8_t read_be8(uint8_t const* ptr)
{
	assert(ptr != nullptr);
	
	return *ptr;
}

//---------------------------------------------------------------------------
// read_be16
//
// Reads a big-endian 16 bit value from memory
//
// Arguments:
//
//	ptr		- Pointer to the data to be read

inline uint16_t read_be16(uint8_t const* ptr)
{
	assert(ptr != nullptr);

	uint16_t val = read_be8(ptr) << 8;
	val |= read_be8(ptr + 1);
	return val;
}

//---------------------------------------------------------------------------
// read_be32
//
// Reads a big-endian 32 bit value from memory
//
// Arguments:
//
//	ptr		- Pointer to the data to be read

inline uint32_t read_be32(uint8_t const* ptr)
{
	assert(ptr != nullptr);

	uint32_t val = read_be16(ptr) << 16;
	val |= read_be16(ptr + 2);
	return val;
}

//---------------------------------------------------------------------------
// dvrstream Constructor (private)
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	buffersize		- Ring buffer size, in bytes
//	readmincount	- Minimum bytes to return from a read operation

dvrstream::dvrstream(char const* url, size_t buffersize, size_t readmincount) :
	m_readmincount(std::max(align::down(readmincount, MPEGTS_PACKET_LENGTH), MPEGTS_PACKET_LENGTH)),
	m_buffersize(align::up(buffersize, 65536))
{
	if(url == nullptr) throw std::invalid_argument("url");

	// Allocate the ring buffer using the 64KiB upward-aligned buffer size
	m_buffer = std::unique_ptr<uint8_t[]>(new uint8_t[m_buffersize]);
	if(!m_buffer) throw std::bad_alloc();

	// Create and initialize the curl multi interface object
	m_curlm = curl_multi_init();
	if(m_curlm == nullptr) throw string_exception(__func__, ": curl_multi_init() failed");

	try {

		// Create and initialize the curl easy interface object
		m_curl = curl_easy_init();
		if(m_curl == nullptr) throw string_exception(__func__, ": curl_easy_init() failed");

		try {

			// Set the options for the easy interface curl handle
			CURLcode curlresult = curl_easy_setopt(m_curl, CURLOPT_URL, url);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERFUNCTION, &dvrstream::curl_responseheaders);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, this);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &dvrstream::curl_write);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_RANGE, "0-");
			if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed: ", curl_easy_strerror(curlresult));

			// Attempt to add the easy handle to the multi handle
			CURLMcode curlmresult = curl_multi_add_handle(m_curlm, m_curl);
			if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_add_handle() failed: ", curl_multi_strerror(curlmresult));

			try {

				// Attempt to begin the data transfer and wait for the HTTP headers to be processed
				if(!transfer_until([&]() -> bool { return m_headers == true; })) throw string_exception(__func__, ": failed to receive HTTP response headers");
			}

			// Remove the easy handle from the multi interface on exception
			catch(...) { curl_multi_remove_handle(m_curlm, m_curl); throw; }
		}

		// Clean up and destroy the easy handle on exception
		catch(...) { curl_easy_cleanup(m_curl); throw; }
	}

	// Clean up and destroy the multi handle on exception
	catch(...) { curl_multi_cleanup(m_curlm); throw; }
}

//---------------------------------------------------------------------------
// dvrstream Destructor

dvrstream::~dvrstream()
{
	close();
}

//---------------------------------------------------------------------------
// dvrstream::canseek
//
// Gets a flag indicating if the stream allows seek operations
//
// Arguments:
//
//	NONE

bool dvrstream::canseek(void) const
{
	return m_canseek;
}

//---------------------------------------------------------------------------
// dvrstream::close
//
// Closes the stream
//
// Arguments:
//
//	NONE

void dvrstream::close(void)
{
	// Remove the easy handle from the multi handle and close them both out
	if((m_curlm != nullptr) && (m_curl != nullptr)) curl_multi_remove_handle(m_curlm, m_curl);
	if(m_curl != nullptr) curl_easy_cleanup(m_curl);
	if(m_curlm != nullptr) curl_multi_cleanup(m_curlm);

	m_curl = nullptr;				// Reset easy handle to null
	m_curlm = nullptr;				// Reset multi handle to null
}

//---------------------------------------------------------------------------
// dvrstream::currenttime
//
// Gets the current playback time based on the presentation timestamps
//
// Arguments:
//
//	NONE

time_t dvrstream::currenttime(void) const
{
	// If either of the presentation timestamps are missing, report zero
	if((m_startpts == 0) || (m_currentpts == 0)) return 0;

	// If the current presentation timestamp is before the start, report zero
	if(m_currentpts < m_startpts) return 0;

	// Calculate the current playback time via the delta between the current
	// and starting presentation timestamp values (90KHz periods)
	uint64_t delta = (m_currentpts - m_startpts) / 90000;
	assert(delta <= static_cast<uint64_t>(std::numeric_limits<time_t>::max()));
	return m_starttime + static_cast<time_t>(delta);
}

//---------------------------------------------------------------------------
// dvrstream::create (static)
//
// Factory method, creates a new dvrstream instance
//
// Arguments:
//
//	url				- URL of the stream to be opened

std::unique_ptr<dvrstream> dvrstream::create(char const* url)
{
	return create(url, DEFAULT_RINGBUFFER_SIZE, DEFAULT_READ_MINCOUNT);
}

//---------------------------------------------------------------------------
// dvrstream::create (static)
//
// Factory method, creates a new dvrstream instance
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	buffersize		- Ring buffer size, in bytes

std::unique_ptr<dvrstream> dvrstream::create(char const* url, size_t buffersize)
{
	return create(url, buffersize, DEFAULT_READ_MINCOUNT);
}

//---------------------------------------------------------------------------
// dvrstream::create (static)
//
// Factory method, creates a new dvrstream instance
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	buffersize		- Ring buffer size, in bytes
//	readmincount	- Minimum bytes to return from a read operation

std::unique_ptr<dvrstream> dvrstream::create(char const* url, size_t buffersize, size_t readmincount)
{
	return std::unique_ptr<dvrstream>(new dvrstream(url, buffersize, readmincount));
}

//---------------------------------------------------------------------------
// dvrstream::curl_responseheaders (static, private)
//
// libcurl callback to process response headers
//
// Arguments:
//
//	data		- Pointer to the response header data
//	size		- Size of a single data element
//	count		- Number of data elements
//	context		- Caller-provided context pointer

size_t dvrstream::curl_responseheaders(char const* data, size_t size, size_t count, void* context)
{
	static const char ACCEPT_RANGES_HEADER[]		= "Accept-Ranges: bytes";
	static const char CONTENT_RANGE_HEADER[]		= "Content-Range: bytes";
	static const char EMPTY_HEADER[]				= "\r\n";
	static const size_t ACCEPT_RANGES_HEADER_LEN	= strlen(ACCEPT_RANGES_HEADER);
	static const size_t CONTENT_RANGE_HEADER_LEN	= strlen(CONTENT_RANGE_HEADER);
	static const size_t EMPTY_HEADER_LEN			= strlen(EMPTY_HEADER);

	size_t cb = size * count;						// Calculate the actual byte count
	if(cb == 0) return 0;							// Nothing to do

	// Cast the context pointer back into a dvrstream instance
	dvrstream* instance = reinterpret_cast<dvrstream*>(context);

	// Accept-Ranges: bytes
	if((cb >= ACCEPT_RANGES_HEADER_LEN) && (strncmp(ACCEPT_RANGES_HEADER, data, ACCEPT_RANGES_HEADER_LEN) == 0)) {

		instance->m_canseek = true;				// Only care if header is present
	}

	// Content-Range: bytes <range-start>-<range-end>/<size>
	// Content-Range: bytes <range-start>-<range-end>/*
	// Content-Range: bytes */<size>
	else if((cb >= CONTENT_RANGE_HEADER_LEN) && (strncmp(CONTENT_RANGE_HEADER, data, CONTENT_RANGE_HEADER_LEN) == 0)) {

		long long start = 0;							// <range-start>
		long long end = MAX_STREAM_LENGTH - 1;			// <range-end>
		long long length = MAX_STREAM_LENGTH;			// <size>

		// Copy the header data into a local buffer to ensure null termination of the string
		std::unique_ptr<char[]> buffer(new char[cb + 1]);
		memcpy(&buffer[0], data, cb);
		buffer[cb] = 0;

		// Attempt to parse a complete Content-Range: header to retrieve all the values, otherwise
		// fall back on just attempting to get the size.  The latter condition occurs on a seek
		// beyond the size of a fixed-length stream, so set the start value to match the size
		int result = sscanf(data, "Content-Range: bytes %lld-%lld/%lld", &start, &end, &length);
		if((result == 0) && (sscanf(data, "Content-Range: bytes */%lld", &length)) == 1) start = length;

		// Reset the stream read/write positions and overall length
		instance->m_startpos = instance->m_readpos = instance->m_writepos = start;
		instance->m_length = length;
	}

	// \r\n (empty header)
	else if((cb >= EMPTY_HEADER_LEN) && (strncmp(EMPTY_HEADER, data, EMPTY_HEADER_LEN) == 0)) {

		// The final header has been processed, indicate that by setting the flag
		instance->m_headers = true;
	}

	return cb;
}

//---------------------------------------------------------------------------
// dvrstream::curl_write (static, private)
//
// libcurl callback to write transferred data into the buffer
//
// Arguments:
//
//	data		- Pointer to the data to be written
//	size		- Size of a single data element
//	count		- Number of data elements
//	context		- Caller-provided context pointer

size_t dvrstream::curl_write(void const* data, size_t size, size_t count, void* context)
{
	size_t				cb = size * count;			// Calculate the actual byte count
	size_t				byteswritten = 0;			// Total bytes actually written

	if((data == nullptr) || (cb == 0) || (context == nullptr)) return 0;

	// Cast the context pointer back into a dvrstream instance
	dvrstream* instance = reinterpret_cast<dvrstream*>(context);

	// This operation requires that all of the data be written, if it isn't going to fit in the
	// available ring buffer space, the input stream has to be paused via CURL_WRITEFUNC_PAUSE
	size_t available = (instance->m_head < instance->m_tail) ? instance->m_tail - instance->m_head : (instance->m_buffersize - instance->m_head) + instance->m_tail;
	if(available < (cb + 1)) { instance->m_paused = true; return CURL_WRITEFUNC_PAUSE; }

	// Write until the buffer has been exhausted or the desired count has been reached
	while(cb) {

		// If the head is behind the tail linearly, take the data between them otherwise
		// take the data between the end of the buffer and the head
		size_t chunk = (instance->m_head < instance->m_tail) ? std::min(cb, instance->m_tail - instance->m_head) : std::min(cb, instance->m_buffersize - instance->m_head);
		memcpy(&instance->m_buffer[instance->m_head], &reinterpret_cast<uint8_t const*>(data)[byteswritten], chunk);

		instance->m_head += chunk;		// Increment the head position
		byteswritten += chunk;			// Increment number of bytes written
		cb -= chunk;					// Decrement remaining bytes

		// If the head has reached the end of the buffer, reset it back to zero
		if(instance->m_head >= instance->m_buffersize) instance->m_head = 0;
	}

	assert(byteswritten == (size * count));		// Verify all bytes were written

	// Increment the number of bytes seen as part of this transfer
	instance->m_writepos += byteswritten;

	return byteswritten;
}

//---------------------------------------------------------------------------
// dvrstream::filter_packets (private)
//
// Applies an mpeg-ts packet filter against the provided packets
//
// Arguments:
//
//	buffer		- Pointer to the mpeg-ts packets to filter
//	count		- Number of mpeg-ts packets provided in the buffer

void dvrstream::filter_packets(uint8_t* buffer, size_t count)
{
	// The packet filter can be disabled completely for a stream if the
	// MPEG-TS packets become misaligned; leaving it enabled might trash things
	if(!m_enablefilter) return;

	// Iterate over all of the packets provided in the buffer
	for(size_t index = 0; index < count; index++) {

		// Set up the pointer to the start of the packet and a working pointer
		uint8_t* packet = buffer + (index * MPEGTS_PACKET_LENGTH);
		uint8_t* current = packet;

		// Read relevant values from the transport stream header
		uint32_t ts_header = read_be32(current);
		uint8_t sync = (ts_header & 0xFF000000) >> 24;
		bool pusi = (ts_header & 0x00400000) == 0x00400000;
		uint16_t pid = static_cast<uint16_t>((ts_header & 0x001FFF00) >> 8);
		bool adaptation = (ts_header & 0x00000020) == 0x00000020;
		bool payload = (ts_header & 0x00000010) == 0x00000010;

		// Check the sync byte, should always be 0x47.  If the packets aren't in sync
		// all kinds of bad things can happen
		assert(sync == 0x47);
		if(sync != 0x47) { 
			
			m_enablefilter = m_enablepcrs = false;		// Stop filtering packets
			m_startpts = m_currentpts = 0;				// Disable PCR reporting

			return;
		}

		// Move the pointer beyond the TS header
		current += 4U;

		// If the packet contains adaptation bytes check for and handle the PCR
		if(adaptation) {

			// Get the adapation field length, this needs to be at least 7 bytes for
			// it to possibly include the PCR value
			uint8_t adaptationlength = read_be8(current);
			if((adaptationlength >= 7) && (m_enablepcrs)) {

				// Only use the first PID on which a PCR has been detected, there may
				// be multiple elementary streams that contain PCR values
				if((m_pcrpid == 0) || (pid == m_pcrpid)) {

					// Check the adaptation flags to see if a PCR is in this packet
					uint8_t adaptationflags = read_be8(current + 1U);
					if((adaptationflags & 0x10) == 0x10) {

						// If the PCR PID hasn't been set, use this PID from now on
						if(m_pcrpid == 0) m_pcrpid = pid;

						// Decode the current PCR using the 90KHz period only, there is 
						// no need to deal with the full 27MHz period
						m_currentpts = decode_pcr90khz(current + 2U);
						if(m_startpts == 0) m_startpts = m_currentpts;

						assert(m_currentpts >= m_startpts);

						// If the current PCR is less than the original PCR value something has
						// gone wrong; disable all PCR detection and reporting on this stream
						if(m_currentpts < m_startpts) {

							m_enablepcrs = false;
							m_startpts = m_currentpts = 0;
						}
					}
				}
			}

			// Move the pointer beyond the adaptation data
			current += adaptationlength;
		}

		// >> PAT
		if((pid == 0x0000) && (payload)) {

			// Align the payload using the pointer provided when pusi is set
			if(pusi) current += read_be8(current) + 1U;

			// Watch out for a TABLEID of 0xFF, this indicates that the remainder
			// of the packet is just stuffed with 0xFF and nothing useful is here
			if(read_be8(current) == 0xFF) continue;

			// Get the first and last section indices and skip to the section data
			uint8_t firstsection = read_be8(current + 6U);
			uint8_t lastsection = read_be8(current + 7U);
			current += 8U;

			// Iterate over all the sections and add the PMT program ids to the set<>
			for(uint8_t section = firstsection; section <= lastsection; section++) {

				uint16_t pmt_program = read_be16(current);
				if(pmt_program != 0) m_pmtpids.insert(read_be16(current + 2U) & 0x1FFF);

				current += 4U;				// Move to the next section
			}
		}

		// >> PMT
		else if((pusi) && (payload) && (m_pmtpids.find(pid) != m_pmtpids.end())) {

			// Get the length of the entire payload to be sure we don't exceed it
			size_t payloadlen = MPEGTS_PACKET_LENGTH - (current - packet);

			uint8_t* pointer = current;			// Get address of current pointer
			current += (*pointer + 1U);			// Align offset with the pointer

			// FILTER: Skip over 0xC0 (SCTE Program Information Message) entries followed immediately
			// by 0x02 (Program Map Table) entries by adjusting the payload pointer and overwriting 0xC0
			if(read_be8(current) == 0xC0) {

				// Acquire the length of the 0xC0 entry, if it exceeds the length of the payload give
				// up -- the + 4 bytes is for the pointer (1), the table id (1) and the length (2)
				uint16_t length = read_be16(current + 1) & 0x3FF;
				if((length + 4U) > payloadlen) break;

				// If the 0xC0 entry is immediately followed by a 0x02 entry, adjust the payload
				// pointer to align to the 0x02 entry and overwrite the 0xC0 entry with filler
				if(read_be8(current + 3U + length) == 0x02) {

					// Take into account any existing pointer value when adjusting it
					*pointer += (3U + static_cast<uint8_t>(length & 0xFF));
					memset(current, 0xFF, 3U + length);
				}
			}
		}

	}	// for(index ...
}

//---------------------------------------------------------------------------
// dvrstream::length
//
// Gets the length of the stream; or -1 if stream is real-time
//
// Arguments:
//
//	NONE

long long dvrstream::length(void) const
{
	return (m_length == MAX_STREAM_LENGTH) ? -1 : m_length;
}

//---------------------------------------------------------------------------
// dvrstream::position
//
// Gets the current position of the stream
//
// Arguments:
//
//	NONE

long long dvrstream::position(void) const
{
	return m_readpos;
}

//---------------------------------------------------------------------------
// dvrstream::read
//
// Reads data from the live stream
//
// Arguments:
//
//	buffer		- Buffer to receive the live stream data
//	count		- Size of the destination buffer in bytes

size_t dvrstream::read(uint8_t* buffer, size_t count)
{
	size_t				bytesread = 0;			// Total bytes actually read
	size_t				available = 0;			// Available bytes to read

	// Verify that the minimum read count has been aligned properly during construction
	assert(m_readmincount == align::down(m_readmincount, MPEGTS_PACKET_LENGTH));
	assert(m_readmincount >= MPEGTS_PACKET_LENGTH);

	if(count >= m_buffersize) throw std::invalid_argument("count");
	if(count == 0) return 0;

	// Transfer data into the ring buffer until the minimum amount of data is available, 
	// the stream has completed, or an exception/error occurs
	transfer_until([&]() -> bool {

		available = (m_tail > m_head) ? (m_buffersize - m_tail) + m_head : m_head - m_tail;
		return (available >= m_readmincount);
	});

	// If there is no available data in the ring buffer after transfer_until, indicate stream is finished
	if(available == 0) return 0;

	// Wait until the first successful read operation to set the start time for the stream
	if(m_starttime == 0) m_starttime = time(nullptr);
	
	// Reads are no longer aligned to return full MPEG-TS packets, determine the offset
	// from the current read position to the first full packet of data
	size_t packetoffset = static_cast<size_t>(align::up(m_readpos, MPEGTS_PACKET_LENGTH) - m_readpos);

	// Starting with the lesser of the amount of data that is available to read and the
	// originally requested count, adjust the end so that it aligns to a full MPEG-TS packet
	count = std::min(available, count);
	if(count >= (packetoffset + MPEGTS_PACKET_LENGTH)) count = packetoffset + align::down(count - packetoffset, MPEGTS_PACKET_LENGTH);

	// Copy the calculated amount of data into the destination buffer
	while(count) {

		// If the tail is behind the head linearly, take the data between them otherwise
		// take the data between the end of the buffer and the tail
		size_t chunk = (m_tail < m_head) ? std::min(count, m_head - m_tail) : std::min(count, m_buffersize - m_tail);
		if(buffer != nullptr) memcpy(&buffer[bytesread], &m_buffer[m_tail], chunk);

		m_tail += chunk;					// Increment the tail position
		bytesread += chunk;					// Increment number of bytes read
		count -= chunk;						// Decrement remaining bytes

		// If the tail has reached the end of the buffer, reset it back to zero
		if(m_tail >= m_buffersize) m_tail = 0;
	}

	m_readpos += bytesread;					// Update the reader position

	// Apply the mpeg-ts packet filter against all complete packets that were read
	if((bytesread >= (packetoffset + MPEGTS_PACKET_LENGTH)) && (buffer != nullptr)) 
		filter_packets(buffer + packetoffset, (bytesread / MPEGTS_PACKET_LENGTH));

	return bytesread;
}

//---------------------------------------------------------------------------
// dvrstream::realtime
//
// Gets a flag indicating if the stream is real-time
//
// Arguments:
//
//	NONE

bool dvrstream::realtime(void) const
{
	return (m_length == MAX_STREAM_LENGTH);
}

//---------------------------------------------------------------------------
// dvrstream::restart (private)
//
// Restarts the stream at the specified position
//
// Arguments:
//
//	position		- Requested starting position for the transfer

long long dvrstream::restart(long long position)
{
	assert(position >= 0);				// Should always be a positive value

	// Remove the easy transfer handle from the multi transfer handle
	CURLMcode curlmresult = curl_multi_remove_handle(m_curlm, m_curl);
	if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_remove_handle() failed: ", curl_multi_strerror(curlmresult));

	// Reset all of the stream state and ring buffer values back to the defaults; leave the 
	// start time and start presentation timestamp values at their original values
	m_paused = m_headers = m_canseek = false;
	m_head = m_tail = 0;
	m_startpos = m_readpos = m_writepos = 0;
	m_length = MAX_STREAM_LENGTH;
	m_currentpts = 0;

	// Format the Range: header value to apply to the transfer object, do not use CURLOPT_RESUME_FROM_LARGE 
	// as it will not insert the request header when the position is zero
	char byterange[32] = { '\0' };
	snprintf(byterange, std::extent<decltype(byterange)>::value, "%lld-", std::max(position, 0LL));

	// Attempt to execute the current transfer operation
	CURLcode curlresult = curl_easy_setopt(m_curl, CURLOPT_RANGE, byterange);
	if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed: ", curl_easy_strerror(curlresult));

	// Add the modified easy transfer handle back to the multi transfer handle and
	// attempt to restart the stream at the specified position
	curlmresult = curl_multi_add_handle(m_curlm, m_curl);
	if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_remove_handle() failed: ", curl_multi_strerror(curlmresult));

	// Execute the data transfer until the HTTP headers have been received and processed
	if(!transfer_until([&]() -> bool { return m_headers == true; })) 
		throw string_exception(__func__, ": failed to receive HTTP response headers");

	return m_readpos;					// Return new starting position of the stream
}

//---------------------------------------------------------------------------
// dvrstream::seek
//
// Sets the stream pointer to a specific position
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

long long dvrstream::seek(long long position, int whence)
{
	long long			newposition = 0;			// New stream position

	// If the stream cannot be seeked, return -1 to indicate the operation is not supported.
	if(!m_canseek) return -1;

	// Calculate the new position of the stream
	if(whence == SEEK_SET) newposition = std::max(position, 0LL);
	else if(whence == SEEK_CUR) newposition = m_readpos + position;
	else if(whence == SEEK_END) newposition = m_length + position;
	else throw std::invalid_argument("whence");

	// Adjust for overflow/underflow of the new position.  If the seek was forward set it
	// to numeric_limits<long long>::max, otherwise set it back to zero
	if(newposition < 0) newposition = (position >= 0) ? std::numeric_limits<long long>::max() : 0;

	// If the calculated position matches the current position there is nothing to do
	if(newposition == m_readpos) return m_readpos;

	// Calculate the minimum stream position currently represented in the ring buffer
	long long minpos = ((m_writepos - m_startpos) > static_cast<long long>(m_buffersize)) ? m_writepos - m_buffersize : m_startpos;

	// If the new position is already represented in the ring buffer, modify the tail pointer to
	// reference that position for the next read operation rather than restarting the stream
	if((newposition >= minpos) && (newposition < m_writepos)) {

		// If the buffer hasn't wrapped around yet, the new tail position is relative to buffer[0]
		if(minpos == m_startpos) m_tail = static_cast<size_t>(newposition - m_startpos);

		else {

			// The buffer has wrapped around at least once, the new tail position is relative to the
			// current head position rather than the start of the buffer
			m_tail = static_cast<size_t>(m_head + (newposition - minpos));
			if(m_tail >= m_buffersize) m_tail -= m_buffersize;

			assert(m_tail <= m_buffersize);				// Verify tail position is valid
		}

		m_readpos = newposition;						// Set the new tail position
		return newposition;								// Successful ring buffer seek
	}

	// Attempt to restart the stream at the calculated position
	return restart(newposition);
}

//---------------------------------------------------------------------------
// dvrstream::starttime
//
// Gets the time at which the stream started
//
// Arguments:
//
//	NONE

time_t dvrstream::starttime(void) const
{
	return m_starttime;
}

//---------------------------------------------------------------------------
// dvrstream::transfer_until (private)
//
// Executes the data transfer until the specified predicate has been satisfied
// or the transfer has completed
//
// Arguments:
//
//	predicate		- Predicate to be satisfied by the transfer

bool dvrstream::transfer_until(std::function<bool(void)> predicate)
{
	int				numfds;				// Number of active file descriptors

	// If the stream has been paused due to the ring buffer filling up, attempt to resume it
	// CAUTION: calling curl_easy_pause() with CURLPAUSE_CONT *immediately* attempts to write 
	// outstanding data into the ring buffer= so when it returns m_paused may have been set 
	// back to true if the ring buffer is still full after attempting to resume
	if(m_paused) {

		m_paused = false;									// Reset the stream paused flag
		curl_easy_pause(m_curl, CURLPAUSE_CONT);			// Resume transfer on the stream
	}

	// If the stream is still paused (buffer is still full) and the predicate can be satisfied,
	// go ahead and let the caller do what it wants to do
	if(m_paused && predicate()) return true;

	// Continue to execute the data transfer until the predicate has been satisfied, the data
	// transfer operation is complete, or the stream has been paused due to a full buffer condition
	CURLMcode curlmresult = curl_multi_perform(m_curlm, &numfds);
	while((curlmresult == CURLM_OK) && (!m_paused) && (numfds > 0) && (predicate() == false)) {

		curlmresult = curl_multi_wait(m_curlm, nullptr, 0, 500, &numfds);
		if(curlmresult == CURLM_OK) curlmresult = curl_multi_perform(m_curlm, &numfds);
	}

	// If a curl error occurred, throw an exception
	if(curlmresult != CURLM_OK) throw string_exception(__func__, ": ", curl_multi_strerror(curlmresult));

	// If the number of file descriptors has reduced to zero, the transfer has completed.
	// Check for an HTTP error response on the transfer and throw an http_exception that
	// will let the caller decide what to do about it
	if(numfds == 0) {

		long responsecode = 200;			// Assume HTTP 200: OK

		// The response code will come back as zero if there was no response from the host,
		// otherwise it should be a standard HTTP response code
		curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &responsecode);

		if(responsecode == 0) throw string_exception("no response from host");
		else if((responsecode < 200) || (responsecode > 299)) throw http_exception(responsecode);
	}

	return predicate();				// Re-evaluate the predicate as the result
}

//---------------------------------------------------------------------------

#pragma warning(pop)
