//---------------------------------------------------------------------------
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
// dvrstream Constructor
//
// Arguments:
//
//	buffersize	- Size in bytes of the stream ring buffer
//	url			- URL of the file stream to be opened

dvrstream::dvrstream(size_t buffersize, char const* url) : m_buffersize(align::up(buffersize, 65536))
{
	if(url == nullptr) throw std::invalid_argument("url");

	// Allocate the ring buffer using the 64KiB upward-aligned buffer size
	m_buffer.reset(new uint8_t[m_buffersize]);
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
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_FAILONERROR, 1L);
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
		m_worker = std::async(std::launch::async, &dvrstream::curl_transfer_func, this, 0ULL);
		m_started.wait_until_equals(true);

		// If the transfer thread failed to initiate the data transfer throw an exception
		if(m_curlresult != CURLE_OK) throw string_exception(__func__, ": failed to open url ", url, ": ", m_curlerr);
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
	m_stop.store(true);				// Signal worker thread to stop
	m_worker.wait();				// Wait for it to actually stop
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
	//
	if((cb >= ACCEPT_RANGES_HEADER_LEN) && (strncmp(ACCEPT_RANGES_HEADER, data, ACCEPT_RANGES_HEADER_LEN) == 0)) {

		instance->m_canseek = true;				// Only care if header is present
	}

	// Content-Range: bytes xxxxxx-yyyyyy/zzzzzz
	// Content-Range: bytes xxxxxx-yyyyyy/*
	// Content-Range: bytes */zzzzzz
	//
	else if((cb >= CONTENT_RANGE_HEADER_LEN) && (strncmp(CONTENT_RANGE_HEADER, data, CONTENT_RANGE_HEADER_LEN) == 0)) {

		unsigned long long start = 0;					// Parsed range start
		unsigned long long end = UINT64_MAX;			// Parsed range end
		unsigned long long length = 0;					// Parsed overall length

		// Copy the header data into a local buffer to ensure null termination of the string
		std::unique_ptr<char[]> buffer(new char[cb + 1]);
		memcpy(&buffer[0], data, cb);
		buffer[cb] = 0;

		// Attempt to parse a complete Content-Range: header and fall back on just the length
		int result = sscanf(data, "Content-Range: bytes %llu-%llu/%llu", &start, &end, &length);
		if(result == 0) sscanf(data, "Content-Range: bytes */%llu", &length);

		// Set the initial stream positions and length if larger than the known length
		instance->m_startpos = instance->m_readpos = instance->m_writepos = start;
		if(length > instance->m_length.load()) instance->m_length.store(length);
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

void dvrstream::curl_transfer_func(unsigned long long position)
{
	// Format the Range: header value to apply to the transfer object, do not use CURLOPT_RESUME_FROM_LARGE 
	// as it will not insert the request header when the position is zero
	char byterange[32];
	snprintf(byterange, std::extent<decltype(byterange)>::value, "%llu-", static_cast<unsigned long long>(position));

	// Attempt to execute the current transfer operation
	m_curlresult = curl_easy_setopt(m_curl, CURLOPT_RANGE, byterange);
	if(m_curlresult == CURLE_OK) m_curlresult = curl_easy_perform(m_curl);

	// If curl_easy_perform fails, m_started will never be signaled by the
	// write callback; signal it now to release any waiting threads
	m_started = true;
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

	// Increment the number of bytes seen as part of this transfer
	instance->m_writepos += byteswritten;

	// Check if the new write position exceeds the known length of the stream, which
	// indicates that the stream is realtime (once set this flag does not get cleared)
	if(instance->m_writepos > instance->m_length.load()) {
		
		instance->m_length.store(instance->m_writepos);
		instance->m_realtime.store(true);
	}

	// Release any thread waiting for the transfer to start *after* some data is
	// available to be read from the buffer to avoid initial reader starvation
	instance->m_started = true;

	return byteswritten;
}

//---------------------------------------------------------------------------
// dvrstream::length
//
// Gets the known length of the stream
//
// Arguments:
//
//	NONE

unsigned long long dvrstream::length(void) const
{
	return m_length.load();
}

//---------------------------------------------------------------------------
// dvrstream::position
//
// Gets the current position of the stream
//
// Arguments:
//
//	NONE

unsigned long long dvrstream::position(void) const
{
	std::unique_lock<std::mutex> lock(m_lock);
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
	size_t				head = 0;				// Current head position
	size_t				tail = 0;				// Current tail position

	if(buffer == nullptr) throw std::invalid_argument("buffer");
	if(count > m_buffersize) throw std::invalid_argument("count");
	if(count == 0) return 0;

	std::unique_lock<std::mutex> lock(m_lock);

	tail = m_buffertail.load();				// Copy the atomic<> tail position
	head = m_bufferhead.load();				// Copy the atomic<> head position

	// Spin until data becomes available in the ring buffer or the stream has stopped
	while(tail == head) {

		// Test the stream worker thread for termination, if it has terminated perform one
		// additional test of the head position to prevent a race condition
		if(m_worker.wait_for(std::chrono::milliseconds(1)) == std::future_status::ready) {

			head = m_bufferhead.load();			// Reload the head position
			if(tail == head) return 0;			// Test it one more time
		}
		
		else head = m_bufferhead.load();		// Reload the head and test again
	}

	// Read until the buffer has been exhausted or the desired count has been reached
	while((tail != head) && (count)) {

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

	return bytesread;
}

//---------------------------------------------------------------------------
// dvrstream::realtime
//
// Flag if the stream is real-time
//
// Arguments:
//
//	NONE

bool dvrstream::realtime(void) const
{
	return m_realtime.load();
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

unsigned long long dvrstream::restart(std::unique_lock<std::mutex> const& lock, unsigned long long position)
{
	// The lock argument is necessary to ensure the caller owns it before proceeding
	if(!lock.owns_lock()) throw string_exception(__func__, ": caller does not own the unique_lock<>");

	// Stop the existing data transfer operation
	m_stop.store(true);
	m_worker.wait();

	// Reinitialize the result code CURL error buffer
	m_curlresult = CURLE_OK;
	memset(m_curlerr, 0, CURL_ERROR_SIZE);

	// Reinitialize the stream control flags
	m_started = false;
	m_paused.store(false);
	m_stop.store(false);

	// Reinitialize the stream information (m_length and m_realtime stay set as-is)
	m_startpos = m_readpos = m_writepos = 0;
	m_canseek = false;

	// Reinitialize the ring buffer back to an empty state
	m_bufferhead.store(0);
	m_buffertail.store(0);

	// Create a new worker thread on which to perform the transfer operations and wait for it to start
	m_worker = std::async(std::launch::async, &dvrstream::curl_transfer_func, this, position);
	m_started.wait_until_equals(true);

	// If the transfer thread failed to initiate the data transfer throw an exception
	if(m_curlresult != CURLE_OK) 
		throw string_exception(__func__, ": failed to restart transfer at position ", position, ": ", m_curlerr);

	// Return the new starting position of the stream, which is not necessarily
	// going to be at the desired seek position
	return m_readpos;
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

unsigned long long dvrstream::seek(long long position, int whence)
{
	unsigned long long		newposition = 0;			// New stream position

	std::unique_lock<std::mutex> lock(m_lock);

	// If the stream cannot be seeked, just return the current position
	if(!m_canseek) return m_readpos;

	// Calculate the new position of the stream, which cannot be negative or exceed the known length
	unsigned long long length = m_length.load();
	if(whence == SEEK_SET) newposition = std::min(static_cast<unsigned long long>(std::max(position, 0LL)), length - 1);
	else if(whence == SEEK_CUR) newposition = std::min(static_cast<unsigned long long>(std::max(m_readpos + position, 0ULL)), length - 1);
	else if(whence == SEEK_END) newposition = std::min(static_cast<unsigned long long>(std::max(length + position, 0ULL)), length - 1);
	else throw std::invalid_argument("whence");

	// If the calculated position matches the current position there is nothing to do
	if(newposition == m_readpos) return m_readpos;

	// Take the write lock to prevent any changes to the head position while calculating
	// if the seek can be fulfilled with the data already in the buffer
	std::unique_lock<std::mutex> writelock(m_writelock);

	// Calculate the minimum stream position currently represented in the ring buffer
	unsigned long long minpos = ((m_writepos - m_startpos) > m_buffersize) ? m_writepos - m_buffersize : m_startpos;

	if((newposition >= minpos) && (newposition <= m_writepos)) {

		// If the buffer hasn't wrapped around yet, the new tail position is relative to buffer[0]
		if(minpos == m_startpos) m_buffertail.store(static_cast<size_t>(newposition - m_startpos));

		else {

			size_t tail = m_bufferhead.load();					// Start at the head (minpos)
			unsigned long long delta = newposition - minpos;	// Calculate the required delta

			// Set the new tail position; if the delta is larger than the remaining space in the
			// buffer it is relative to buffer[0], otherwise it is relative to buffer[minpos]
			m_buffertail.store(static_cast<size_t>((delta >= (m_buffersize - tail)) ? delta - (m_buffersize - tail) : tail + delta));
		}

		m_readpos = newposition;						// Set the new tail position
		return newposition;								// Successful ring buffer seek
	}

	// Ring buffer seek was unsuccessful, release the writer lock
	writelock.unlock();

	// Attempt to restart the stream at the calculated position
	return restart(lock, newposition);
}

//---------------------------------------------------------------------------

#pragma warning(pop)
