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
#include "livestream.h"

#include <algorithm>
#include <chrono>
#include <string.h>
#include <type_traits>

#include "string_exception.h"

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// livestream Constructor
//
// Arguments:
//
//	buffersize	- Size in bytes of the stream ring buffer

livestream::livestream(size_t buffersize) : m_buffersize(buffersize), m_buffer(new uint8_t[buffersize])
{
	if(!m_buffer) throw std::bad_alloc();
}

//---------------------------------------------------------------------------
// livestream Destructor

livestream::~livestream()
{
	stop();							// Stop the live stream
}

//---------------------------------------------------------------------------
// livestream::curl_responseheaders (static, private)
//
// libcurl callback to process response headers
//
// Arguments:
//
//	data		- Pointer to the response header data
//	size		- Size of a single data element
//	count		- Number of data elements
//	context		- Caller-provided context pointer

size_t livestream::curl_responseheaders(char const* data, size_t size, size_t count, void* context)
{
	static const char CONTENT_RANGE_HEADER[]		= "Content-Range:";
	static const size_t CONTENT_RANGE_HEADER_LEN	= strlen(CONTENT_RANGE_HEADER);

	size_t cb = size * count;				// Calculate the actual byte count
	if(cb == 0) return 0;					// Nothing to do

	// Cast the context pointer back into a livestream instance
	livestream* instance = reinterpret_cast<livestream*>(context);

	// Content-Range:
	//
	if((cb >= CONTENT_RANGE_HEADER_LEN) && (strncmp(CONTENT_RANGE_HEADER, data, CONTENT_RANGE_HEADER_LEN) == 0)) {

		unsigned long long rangestart = 0;

		// Copy the header data into a local buffer to ensure null termination, which is not guaranteed
		std::unique_ptr<char[]> buffer(new char[cb + 1]);
		memcpy(&buffer[0], data, cb);
		buffer[cb] = 0;

		// The Content-Range header gives us the starting position of the stream from the server's
		// perspective which is used to normalize the reported stream position
		if(sscanf(data, "Content-Range: bytes %llu-", &rangestart) == 1) {
			
			// When the stream starts (or is restarted) set the read/write positions
			instance->m_readpos = instance->m_writepos = rangestart;
		}
	}

	return cb;
}

//---------------------------------------------------------------------------
// livestream::curl_transfercontrol (static, private)
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

int livestream::curl_transfercontrol(void* context, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
	// Cast the livestream instance pointer out from the context pointer
	livestream* instance = reinterpret_cast<livestream*>(context);

	// If a stop has been signaled, terminate the transfer
	if(instance->m_stop.exchange(false) == true) return -1;

	// Automatically resume a paused data transfer when this notification callback is invoked
	if(instance->m_paused.exchange(false) == true) curl_easy_pause(instance->m_curl, CURLPAUSE_CONT);

	return 0;
}

//---------------------------------------------------------------------------
// livestream::curl_write (static, private)
//
// libcurl callback to write transferred data into the buffer
//
// Arguments:
//
//	data		- Pointer to the data to be written
//	size		- Size of a single data element
//	count		- Number of data elements
//	context		- Caller-provided context pointer

size_t livestream::curl_write(void const* data, size_t size, size_t count, void* context)
{
	size_t				cb = size * count;			// Calculate the actual byte count
	size_t				byteswritten = 0;			// Total bytes actually written

	if((data == nullptr) || (cb == 0) || (context == nullptr)) return 0;

	// Cast the context pointer back into a livestream instance
	livestream* instance = reinterpret_cast<livestream*>(context);

	// If a stop has been signaled, terminate the transfer now rather than waiting for curl_transfercontrol
	if(instance->m_stop.exchange(false) == true) return 0;

	// Copy the current head and tail positions, this works without a lock
	// by operating on the state of the buffer at the time of the request
	size_t head = instance->m_bufferhead;
	size_t tail = instance->m_buffertail;

	// This operation requires that all of the data be written, if it isn't going to fit
	// in the available ring buffer space, the input stream has to be paused via CURL_WRITEFUNC_PAUSE
	size_t available = (head < tail) ? tail - head : (instance->m_buffersize - head) + tail;
	if((instance->m_bufferfull) || (available < cb)) { instance->m_paused.store(true); return CURL_WRITEFUNC_PAUSE; }

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

		// If the head has reached the tail, the buffer is now full
		if(head == tail) { instance->m_bufferfull = true; break; }
	}

	// Modify the atomic<> head position after the operation has completed and notify
	// that a write operation against the ring buffer has completed (head has changed)
	instance->m_bufferhead.store(head);
	instance->m_bufferhasdata.notify_all();

	// All of the data should have been written into the ring buffer
	assert(byteswritten == (size * count));

	// Increment the number of bytes seen as part of this transfer, and if
	// we have exceeded the previously known length update that as well
	instance->m_writepos += byteswritten;
	if(instance->m_writepos > instance->m_length.load()) instance->m_length.store(instance->m_writepos);

	// Release the thread waiting for the transfer to start after some data is
	// available to be read from the buffer to avoid initial reader starvation
	instance->m_started = true;

	return byteswritten;
}

//---------------------------------------------------------------------------
// livestream::length
//
// Gets the length of the live stream as transferred thus far
//
// Arguments:
//
//	NONE

uint64_t livestream::length(void) const
{
	return m_length.load();
}
	
//---------------------------------------------------------------------------
// livestream::position
//
// Gets the current position within the live stream
//
// Arguments:
//
//	NONE

uint64_t livestream::position(void) const
{
	std::unique_lock<std::mutex> lock(m_lock);

	return m_readpos;
}

//---------------------------------------------------------------------------
// livestream::read
//
// Reads data from the live stream
//
// Arguments:
//
//	buffer		- Buffer to receive the live stream data
//	count		- Size of the destination buffer in bytes
//	timeoutms	- Amount of time to wait for the read to succeed

size_t livestream::read(uint8_t* buffer, size_t count, uint32_t timeoutms)
{
	size_t				bytesread = 0;			// Total bytes actually read
	size_t				head = 0;				// Current head position
	size_t				tail = 0;				// Current tail position

	if(buffer == nullptr) throw std::invalid_argument("buffer");
	if(count > m_buffersize) throw std::invalid_argument("count");
	if(count == 0) return 0;

	std::unique_lock<std::mutex> lock(m_lock);

	// Wait up to the specified amount of time for any data to be available in the ring buffer
	if(!m_bufferhasdata.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutms), [&]() -> bool { 
	
		head = m_bufferhead;				// Copy the atomic<> head position
		tail = m_buffertail;				// Copy the atomic<> tail position

		// If the head and the tail are in the same position the buffer may be empty
		if((head == tail) && (m_bufferempty)) return false;

		return true;						// Data is available

	})) return 0;

	// Read until the buffer has been exhausted or the desired count has been reached
	while(count) {

		// If the tail is behind the head linearly, take the data between them otherwise
		// take the data between the end of the buffer and the tail
		size_t chunk = (tail < head) ? std::min(count, head - tail) : std::min(count, m_buffersize - tail);
		memcpy(&buffer[bytesread], &m_buffer[tail], chunk);

		tail += chunk;					// Increment the tail position
		bytesread += chunk;				// Increment number of bytes read
		count -= chunk;					// Decrement remaining bytes

		// If the tail has reached the end of the buffer, reset it back to zero
		if(tail >= m_buffersize) tail = 0;

		// If the tail has reached the head, the buffer is now empty
		if(tail == head) { m_bufferempty = true; break; }
	}

	// Modify the atomic<> tail position after the operation has completed
	m_buffertail.store(tail);

	// Increment the current position of the stream beyond what was read
	m_readpos += bytesread;

	return bytesread;
}

//---------------------------------------------------------------------------
// livestream::reset_stream_state (private)
//
// Resets the state of the live stream transfer
//
// Arguments:
//
//	lock		- Reference to unique_lock<> that must be owned

void livestream::reset_stream_state(std::unique_lock<std::mutex> const& lock)
{
	// The lock argument is necessary to ensure the caller owns it before proceeding
	if(!lock.owns_lock()) throw string_exception(__func__, ": caller does not own the unique_lock<>");

	// The worker thread must not be running when the stream state is reset
	if(m_worker.joinable()) throw string_exception(__func__, ": cannot reset an active data transfer");

	// Reset the stream control flags
	m_started = false;
	m_paused.store(false);
	m_stop.store(false);

	// Reset the stream positions (leave length intact)
	m_readpos = m_writepos = 0;

	// Reset the ring buffer back to an empty state
	m_bufferempty = true;
	m_bufferfull = false;
	m_bufferhead = 0;
	m_buffertail = 0;
}

//---------------------------------------------------------------------------
// livestream::seek
//
// Stops and restarts the data transfer at a specific position
//
// Arguments:
//
//	position		- Requested position for the seek operation

uint64_t livestream::seek(uint64_t position)
{
	// Format the RANGE header value to be applied to the new request
	char byterange[32];
	snprintf(byterange, std::extent<decltype(byterange)>::value, "%llu-", static_cast<unsigned long long>(position));

	std::unique_lock<std::mutex> lock(m_lock);

	// If the position is the same as the current position, there is nothing to do
	if(position == m_readpos) return position;

	// The transfer must be active prior to the seek operation
	if(!m_worker.joinable()) throw string_exception(__func__, ": cannot seek an inactive data transfer");

	m_stop.store(true);					// Signal worker thread to stop
	m_worker.join();					// Wait for it to actually stop

	reset_stream_state(lock);			// Reset the stream state

	// The only option that gets changed on the original transfer object is RANGE
	CURLcode curlresult = curl_easy_setopt(m_curl, CURLOPT_RANGE, byterange);
	if(curlresult != CURLE_OK) { 
	
		// If CURLOPT_RANGE couldn't be applied to the existing transfer object stop the transfer
		// by closing out the existing CURL object as would be done by the stop() method

		curl_easy_cleanup(m_curl);
		m_curl = nullptr;

		throw string_exception(__func__, ": curl_easy_setopt() failed; transfer stopped");
	}

	// Create a worker thread on which to perform the transfer operations
	m_worker = std::move(std::thread([=]() { curl_easy_perform(m_curl); m_started = true; }));

	// Wait for some data to be delivered on the stream (or the worker to die prematurely)
	m_started.wait_until_equals(true);

	// Return the starting position of the stream
	return m_readpos;
}

//---------------------------------------------------------------------------
// livestream::start
//
// Arguments:
//
//	url			- URL of the live stream to be opened

uint64_t livestream::start(char const* url)
{
	if(url == nullptr) throw std::invalid_argument("url");

	std::unique_lock<std::mutex> lock(m_lock);

	// Check to make sure that the worker thread isn't already running
	if(m_worker.joinable()) throw string_exception(__func__, ": data transfer is already active");

	// Create and initialize the libcurl easy interface for the specified URL
	m_curl = curl_easy_init();
	if(m_curl == nullptr) throw string_exception(__func__, ": curl_easy_init() failed");

	try {

		// Set the options for the easy interface curl object
		CURLcode curlresult = curl_easy_setopt(m_curl, CURLOPT_URL, url);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_FAILONERROR, 1L);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERFUNCTION, &livestream::curl_responseheaders);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, this);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &livestream::curl_write);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_XFERINFOFUNCTION, &livestream::curl_transfercontrol);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_XFERINFODATA, this);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_NOPROGRESS, 0L);
		if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed");

		// Create a worker thread on which to perform the transfer operations
		m_worker = std::move(std::thread([=]() { curl_easy_perform(m_curl); m_started = true; }));

		// Wait for some data to be delivered on the stream (or the worker to die prematurely)
		m_started.wait_until_equals(true);

		// Return the starting position of the stream
		return m_readpos;
	}
		
	catch(...) { curl_easy_cleanup(m_curl); m_curl = nullptr; throw; }
}

//---------------------------------------------------------------------------
// livestream::stop
//
// Stops the data transfer into the live stream buffer
//
// Arguments:
//
//	NONE

uint64_t livestream::stop(void)
{
	uint64_t			position;			// Position at which stream stopped

	std::unique_lock<std::mutex> lock(m_lock);

	// If the worker thread is not running, the transfer has already stopped; don't
	// throw an exception just return a nice peaceful zero to the caller
	if(!m_worker.joinable()) return 0;

	// Signal the worker thread to stop and wait for it to do so
	m_stop.store(true);
	m_worker.join();

	// Grab the final position of the stream before resetting
	position = m_readpos;

	// Reset the stream state
	reset_stream_state(lock);

	// Clean up the CURL easy interface object
	curl_easy_cleanup(m_curl);
	m_curl = nullptr;

	// Return the final position of the stream
	return position;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
