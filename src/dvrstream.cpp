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
#include <chrono>
#include <string.h>

#include "align.h"
#include "string_exception.h"

#pragma warning(push, 4)

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

// dvrstream::DEFAULT_READ_MIN (static)
//
// Default minimum amount of data to return from a read request
size_t const dvrstream::DEFAULT_READ_MINCOUNT = (1 KiB);

// dvrstream::DEFAULT_READ_TIMEOUT_MS (static)
//
// Default amount of time for a read operation to succeed
unsigned int const dvrstream::DEFAULT_READ_TIMEOUT_MS = 2500;

// dvrstream::DEFAULT_RINGBUFFER_SIZE (static)
//
// Default ring buffer size, in bytes
size_t const dvrstream::DEFAULT_RINGBUFFER_SIZE = (4 MiB);

// dvrstream::MAX_STREAM_LENGTH (static)
//
// Maximum allowable stream length; indicates a real-time stream
long long const dvrstream::MAX_STREAM_LENGTH = std::numeric_limits<long long>::max();

// dvrstream::MPEGTS_PACKET_LENGTH (static)
//
// Length of a single mpeg-ts data packet
size_t const dvrstream::MPEGTS_PACKET_LENGTH = 188;
	
//---------------------------------------------------------------------------
// dvrstream Constructor (private)
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	buffersize		- Ring buffer size, in bytes
//	readmincount	- Minimum bytes to return from a read operation
//	readtimeout		- Read operation timeout, in millseconds

dvrstream::dvrstream(char const* url, size_t buffersize, size_t readmincount, unsigned int readtimeout) :
	m_readmincount(std::max(align::down(readmincount, MPEGTS_PACKET_LENGTH), MPEGTS_PACKET_LENGTH)),
	m_readtimeout(std::max(1U, readtimeout)), m_buffersize(align::up(buffersize, 65536))
{
	// m_readmincount is aligned downward to a mpeg-ts packet boundary with a minimum of one packet
	// m_readtimeout has a minimum value of one millisecond
	// m_buffersize is aligned upward to a 64KiB boundary

	if(url == nullptr) throw std::invalid_argument("url");

	// Allocate the ring buffer using the 64KiB upward-aligned buffer size
	m_buffer = std::unique_ptr<uint8_t[]>(new uint8_t[m_buffersize]);
	if(!m_buffer) throw std::bad_alloc();

	// Initialize the CURL error string buffer to all nulls before starting up
	memset(m_curlerr, 0, CURL_ERROR_SIZE);

	// Create and initialize the curl easy interface object
	m_curl = curl_easy_init();
	if(m_curl == nullptr) throw string_exception(__func__, ": curl_easy_init() failed");

	try {

		// Set the general options for the easy interface curl object
		CURLcode curlresult = curl_easy_setopt(m_curl, CURLOPT_URL, url);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERFUNCTION, &dvrstream::curl_responseheaders);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, this);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &dvrstream::curl_write);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_XFERINFOFUNCTION, &dvrstream::curl_transfercontrol);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_XFERINFODATA, this);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_NOPROGRESS, 0L);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_ERRORBUFFER, m_curlerr);
		if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed");

		// Create a new worker thread on which to perform the transfer operations and wait for it to start
		m_worker = std::thread(&dvrstream::curl_transfer_func, this, 0LL);
		m_started.wait_until_equals(true);

		// If the transfer thread failed to initiate the data transfer throw an exception
		if((m_curlresult != CURLE_OK) && (m_curlresult != CURLE_ABORTED_BY_CALLBACK)) {
		
			m_stop.store(true);				// Signal worker to stop (shouldn't be running)
			m_worker.join();				// Wait for the worker thread to stop

			throw string_exception(__func__, ": failed to open url ", url, ": ", m_curlerr);
		}
	}

	// Clean up the CURL easy interface object on exception
	catch(...) { curl_easy_cleanup(m_curl); throw; }
}

//---------------------------------------------------------------------------
// dvrstream Destructor

dvrstream::~dvrstream()
{
	// Close out the data transfer if it's still active
	try { close(); }
	catch(...) { /* DO NOTHING */ }

	// Clean up the curl easy interface object
	curl_easy_cleanup(m_curl);
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
	std::unique_lock<std::mutex> lock(m_lock);
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
	if(!m_worker.joinable()) return;	// Thread is not running

	m_stop.store(true);					// Signal worker thread to stop
	m_worker.join();					// Wait for it to actually stop
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
	return create(url, DEFAULT_RINGBUFFER_SIZE, DEFAULT_READ_MINCOUNT, DEFAULT_READ_TIMEOUT_MS);
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
	return create(url, buffersize, DEFAULT_READ_MINCOUNT, DEFAULT_READ_TIMEOUT_MS);
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
	return create(url, buffersize, readmincount, DEFAULT_READ_TIMEOUT_MS);
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
//	readtimeout		- Read operation timeout, in millseconds

std::unique_ptr<dvrstream> dvrstream::create(char const* url, size_t buffersize, size_t readmincount, unsigned int readtimeout)
{
	return std::unique_ptr<dvrstream>(new dvrstream(url, buffersize, readmincount, readtimeout));
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
	static const size_t ACCEPT_RANGES_HEADER_LEN	= strlen(ACCEPT_RANGES_HEADER);
	static const size_t CONTENT_RANGE_HEADER_LEN	= strlen(CONTENT_RANGE_HEADER);

	size_t cb = size * count;				// Calculate the actual byte count
	if(cb == 0) return 0;					// Nothing to do

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

	return cb;
}

//---------------------------------------------------------------------------
// dvrstream::curl_transfercontrol (static, private)
//
// libcurl callback to handle transfer information/progress
//
// Arguments:
//
//	context		- Caller-provided context pointer
//	dltotal		- Number of bytes expected to be downloaded
//	dlnow		- Number of bytes already downloaded
//	ultotal		- Number of bytes expected to be uploaded
//	ulnow		- Number of bytes already uploaded

int dvrstream::curl_transfercontrol(void* context, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
	// Cast the dvrstream instance pointer out from the context pointer
	dvrstream* instance = reinterpret_cast<dvrstream*>(context);

	// If a stop has been signaled, terminate the transfer
	if(instance->m_stop.exchange(false) == true) return -1;

	// Automatically resume a paused data transfer when this notification callback is invoked
	if(instance->m_paused.exchange(false) == true) curl_easy_pause(instance->m_curl, CURLPAUSE_CONT);

	return 0;
}

//---------------------------------------------------------------------------
// dvrstream::curl_transfer_func (private)
//
// Worker thread procedure for the CURL data transfer
//
// Arguments:
//
//	position		- Requested starting position for the transfer

void dvrstream::curl_transfer_func(long long position)
{
	assert(position >= 0);				// Should always be a positive value

	// Format the Range: header value to apply to the transfer object, do not use CURLOPT_RESUME_FROM_LARGE 
	// as it will not insert the request header when the position is zero
	char byterange[32];
	snprintf(byterange, std::extent<decltype(byterange)>::value, "%lld-", std::max(position, 0LL));

	// Attempt to execute the current transfer operation
	m_curlresult = curl_easy_setopt(m_curl, CURLOPT_RANGE, byterange);
	if(m_curlresult == CURLE_OK) m_curlresult = curl_easy_perform(m_curl);

	// If curl_easy_perform fails, m_started will never be signaled by the
	// write callback; signal it now to release any waiting threads
	m_started = true;

	// When the thread has completed, set the stopped flag and signal the condvar
	// to release any in-progress read() that is waiting for data
	m_stopped.store(true);
	m_cv.notify_all();
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

	// To support seeking within the ring buffer, some level of synchronization
	// is required -- there should be almost zero contention on this lock
	std::unique_lock<std::mutex> writelock(instance->m_writelock);

	// Copy the current head and tail positions, this works without a lock by operating
	// on the state of the buffer at the time of the request
	size_t head = instance->m_bufferhead.load();
	size_t tail = instance->m_buffertail.load();

	// This operation requires that all of the data be written, if it isn't going to fit in the
	// available ring buffer space, the input stream has to be paused via CURL_WRITEFUNC_PAUSE
	size_t available = (head < tail) ? tail - head : (instance->m_buffersize - head) + tail;
	if(available < (cb + 1)) { instance->m_paused.store(true); return CURL_WRITEFUNC_PAUSE; }

	// Write until the buffer has been exhausted or the desired count has been reached
	while(cb) {

		// If the head is behind the tail linearly, take the data between them otherwise
		// take the data between the end of the buffer and the head
		size_t chunk = (head < tail) ? std::min(cb, tail - head) : std::min(cb, instance->m_buffersize - head);
		memcpy(&instance->m_buffer[head], &reinterpret_cast<uint8_t const*>(data)[byteswritten], chunk);

		head += chunk;					// Increment the head position
		byteswritten += chunk;			// Increment number of bytes written
		cb -= chunk;					// Decrement remaining bytes

		// If the head has reached the end of the buffer, reset it back to zero
		if(head >= instance->m_buffersize) head = 0;
	}

	assert(byteswritten == (size * count));		// Verify all bytes were written
	instance->m_bufferhead.store(head);			// Modify atomic<> head position
	instance->m_cv.notify_all();				// Data has been written

	// Increment the number of bytes seen as part of this transfer
	instance->m_writepos += byteswritten;

	// Release any thread waiting for the transfer to start *after* some data is
	// available to be read from the buffer to avoid initial reader starvation
	instance->m_started = true;

	return byteswritten;
}

//---------------------------------------------------------------------------
// dvrstream::filter_packets (private)
//
// Applies an mpeg-ts packet filter against the provided packets
//
// Arguments:
//
//	lock		- Reference to unique_lock<> that must be owned
//	buffer		- Pointer to the mpeg-ts packets to filter
//	count		- Number of mpeg-ts packets provided in the buffer

void dvrstream::filter_packets(std::unique_lock<std::mutex> const& lock, uint8_t* buffer, size_t count)
{
	// The lock argument is necessary to ensure the caller owns it before proceeding
	if(!lock.owns_lock()) throw string_exception(__func__, ": caller does not own the unique_lock<>");

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

		// Check the sync byte, should always be 0x47.  If the packets aren't
		// in sync, abort the operation -- they will all be out of sync
		assert(sync == 0x47);
		if(sync != 0x47) return;

		// Skip over the header and any adaptation bytes
		current += 4U;
		if(adaptation) current += read_be8(current);

		// >> PAT
		if((pid == 0x0000) && (payload)) {

			// Align the payload using the pointer provided when pusi is set
			if(pusi) current += read_be8(current) + 1U;

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
		if((pusi) && (payload) && (m_pmtpids.find(pid) != m_pmtpids.end())) {

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
	std::unique_lock<std::mutex> lock(m_lock);
	return (m_length == MAX_STREAM_LENGTH) ? -1 : m_length;
}

//---------------------------------------------------------------------------
// dvrstream::position
//
// Gets the current position of the stream; or -1 if stream is real-time
//
// Arguments:
//
//	NONE

long long dvrstream::position(void) const
{
	std::unique_lock<std::mutex> lock(m_lock);
	return (m_length == MAX_STREAM_LENGTH) ? -1 : m_readpos;
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
	size_t				head = 0;				// Current head position
	size_t				tail = 0;				// Current tail position
	size_t				available = 0;			// Available bytes to read
	bool				stopped = false;		// Flag if data transfer has stopped

	// Verify that the minimum read count has been aligned properly during construction
	assert(m_readmincount == align::down(m_readmincount, MPEGTS_PACKET_LENGTH));
	assert(m_readmincount >= MPEGTS_PACKET_LENGTH);

	// Verify that the read timeout is at least one millisecond
	assert(m_readtimeout >= 1U);

	if(buffer == nullptr) throw std::invalid_argument("buffer");
	if(count > m_buffersize) throw std::invalid_argument("count");
	if(count == 0) return 0;

	std::unique_lock<std::mutex> lock(m_lock);

	// Wait up to the timeout for there to be the minimim amount of available data in the
	// buffer, if there is not the condvar will be triggered on the next write or a thread stop.
	if(m_cv.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(m_readtimeout), [&]() -> bool { 

		tail = m_buffertail.load();				// Copy the atomic<> tail position
		head = m_bufferhead.load();				// Copy the atomic<> head position
		stopped = m_stopped.load();				// Copy the atomic<> stopped flag

		// Calculate the amount of space available in the buffer
		available = (tail > head) ? (m_buffersize - tail) + head : head - tail;
		
		// The result from the predicate is true if enough data or stopped
		return ((available >= m_readmincount) || (stopped));
	
	}) == false) return 0;

	// If the wait loop was broken by the worker thread stopping, make one more pass
	// to ensure that no additional data was first written by the thread
	if((available < m_readmincount) && (stopped)) {

		tail = m_buffertail.load();				// Copy the atomic<> tail position
		head = m_bufferhead.load();				// Copy the atomic<> head position

		// Calculate the amount of space available in the buffer
		available = (tail > head) ? (m_buffersize - tail) + head : head - tail;
	}

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
		size_t chunk = (tail < head) ? std::min(count, head - tail) : std::min(count, m_buffersize - tail);
		memcpy(&buffer[bytesread], &m_buffer[tail], chunk);

		tail += chunk;						// Increment the tail position
		bytesread += chunk;					// Increment number of bytes read
		count -= chunk;						// Decrement remaining bytes

		// If the tail has reached the end of the buffer, reset it back to zero
		if(tail >= m_buffersize) tail = 0;
	}

	m_buffertail.store(tail);				// Modify atomic<> tail position
	m_readpos += bytesread;					// Update the reader position

	// Apply the mpeg-ts packet filter against all complete packets that were read
	if(bytesread >= (packetoffset + MPEGTS_PACKET_LENGTH)) 
		filter_packets(lock, buffer + packetoffset, (bytesread / MPEGTS_PACKET_LENGTH));

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
	std::unique_lock<std::mutex> lock(m_lock);
	return (m_length == MAX_STREAM_LENGTH);
}

//---------------------------------------------------------------------------
// dvrstream::restart (private)
//
// Restarts the stream at the specified position
//
// Arguments:
//
//	lock			- Reference to unique_lock<> that must be owned
//	position		- Requested starting position for the transfer

long long dvrstream::restart(std::unique_lock<std::mutex>& lock, long long position)
{
	// The lock argument is necessary to ensure the caller owns it before proceeding
	if(!lock.owns_lock()) throw string_exception(__func__, ": caller does not own the unique_lock<>");

	// Stop the existing data transfer operation if necessary
	if(m_worker.joinable()) {

		m_stop.store(true);				// Signal the thread to stop the transfer

		// It's somewhat faster to wait for the worker thread to signal m_stopped and then
		// just let the thread die naturally than it is to join it here; saves a few milliseconds
		m_cv.wait(lock, [&]() -> bool { return m_stopped.load(); });
		m_worker.detach();
	}

	// Reinitialize the result code CURL error buffer
	m_curlresult = CURLE_OK;
	memset(m_curlerr, 0, CURL_ERROR_SIZE);

	// Reinitialize the stream control flags
	m_started = false;
	m_stopped.store(false);
	m_paused.store(false);
	m_stop.store(false);

	// Reinitialize the stream information
	m_startpos = m_readpos = m_writepos = m_length = 0;
	m_canseek = false;

	// Reinitialize the ring buffer back to an empty state
	m_bufferhead.store(0);
	m_buffertail.store(0);

	// Create a new worker thread on which to perform the transfer operations and wait for it to start
	m_worker = std::thread(&dvrstream::curl_transfer_func, this, position);
	m_started.wait_until_equals(true);

	// If the transfer thread failed to initiate the data transfer throw an exception
	if((m_curlresult != CURLE_OK) && (m_curlresult != CURLE_ABORTED_BY_CALLBACK)) {

		m_stop.store(true);				// Signal worker to stop (shouldn't be running)
		m_worker.join();				// Wait for the worker thread to stop

		throw string_exception(__func__, ": failed to restart transfer at position ", position, ": ", m_curlerr);
	}

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

	std::unique_lock<std::mutex> lock(m_lock);

	// If the stream cannot be seeked, just return the current position
	if(!m_canseek) return m_readpos;

	// Calculate the new position of the stream
	if(whence == SEEK_SET) newposition = std::max(position, 0LL);
	else if(whence == SEEK_CUR) newposition = m_readpos + position;
	else if(whence == SEEK_END) newposition = m_length + position;
	else throw std::invalid_argument("whence");

	// Adjust for overflow/underflow of the new position.  If the seek was forward set it
	// to numeric_limits<long long>::max, otherwise set it back to zero
	if(newposition < 0) newposition = (position > 0) ? std::numeric_limits<long long>::max() : 0;

	// If the calculated position matches the current position there is nothing to do
	if(newposition == m_readpos) return m_readpos;

	// Take the write lock to prevent any changes to the head position while calculating
	// if the seek can be fulfilled with the data already in the buffer
	std::unique_lock<std::mutex> writelock(m_writelock);

	// Calculate the minimum stream position currently represented in the ring buffer
	long long minpos = ((m_writepos - m_startpos) > static_cast<long long>(m_buffersize)) ? m_writepos - m_buffersize : m_startpos;

	if((newposition >= minpos) && (newposition <= m_writepos)) {

		// If the buffer hasn't wrapped around yet, the new tail position is relative to buffer[0]
		if(minpos == m_startpos) m_buffertail.store(static_cast<size_t>(newposition - m_startpos));

		else {

			size_t tail = m_bufferhead.load();				// Start at the head (minpos)
			long long delta = newposition - minpos;			// Calculate the required delta

			// Set the new tail position; if the delta is larger than the remaining space in the
			// buffer it is relative to buffer[0], otherwise it is relative to buffer[minpos]
			m_buffertail.store(static_cast<size_t>((delta >= static_cast<long long>(m_buffersize - tail)) ? delta - (m_buffersize - tail) : tail + delta));
		}

		m_readpos = newposition;						// Set the new tail position
		return newposition;								// Successful ring buffer seek
	}

	// Ring buffer seek was unsuccessful, release the lock
	writelock.unlock();

	// Attempt to restart the stream at the calculated position
	return restart(lock, newposition);
}

//---------------------------------------------------------------------------

#pragma warning(pop)
