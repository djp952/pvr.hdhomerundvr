//---------------------------------------------------------------------------
// Copyright (c) 2016-2021 Michael G. Brehm
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
#include "xmlstream.h"

#include <algorithm>
#include <assert.h>

#include "http_exception.h"
#include "string_exception.h"

#pragma warning(push, 4)

// xmlstream::DEFAULT_RINGBUFFER_SIZE (static)
//
// Default ring buffer size, in bytes
size_t const xmlstream::DEFAULT_RINGBUFFER_SIZE = (1 MiB);

//---------------------------------------------------------------------------
// curl_multi_get_result
//
// Retrieves the result from a cURL easy handle assigned to a multi handle
//
// Arguments:
//
//	multi		- cURL multi interface handle
//	easy		- cURL easy interface handle
//	result		- On success, receives the easy handle result code

static bool curl_multi_get_result(CURLM* multi, CURL* easy, CURLcode *result)
{
	int					nummessages;			// Number of messages in the queue
	struct CURLMsg*		msg = nullptr;			// Pointer to next cURL message

	assert((multi != nullptr) && (easy != nullptr) && (result != nullptr));

	*result = CURLE_OK;					// Initialize [out] variable

	// Read the first informational message from the queue
	msg = curl_multi_info_read(multi, &nummessages);
	while(msg != nullptr) {

		// If this message applies to the easy handle and indicates DONE, return the result
		if((msg->easy_handle == easy) && (msg->msg == CURLMSG_DONE)) {

			*result = msg->data.result;
			return true;
		}

		// Iterate to the next message in the queue
		msg = curl_multi_info_read(multi, &nummessages);
	}

	return false;
}

//---------------------------------------------------------------------------
// xmlstream Constructor (private)
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	useragent		- User-Agent string to specify for the connection
//	share			- CURLSH instance to use for the connection

xmlstream::xmlstream(char const* url, char const* useragent, CURLSH* share) : m_buffersize(DEFAULT_RINGBUFFER_SIZE)
{
	size_t		available = 0;				// Amount of available ring buffer data

	if(url == nullptr) throw std::invalid_argument("url");

	// Allocate the ring buffer using the 64KiB upward-aligned buffer size
	m_buffer = std::unique_ptr<uint8_t[]>(new uint8_t[m_buffersize]);
	if(!m_buffer) throw std::bad_alloc();

	// Allocate and initialize the cURL handle error message buffer
	m_curlerr = std::unique_ptr<char[]>(new char[CURL_ERROR_SIZE + 1]);
	if(!m_curlerr) throw std::bad_alloc();
	memset(m_curlerr.get(), 0, CURL_ERROR_SIZE + 1);

	// Create and initialize the curl multi interface object
	m_curlm = curl_multi_init();
	if(m_curlm == nullptr) throw string_exception(__func__, ": curl_multi_init() failed");

	try {

		// Disable pipelining/multiplexing on the multi interface object
		CURLMcode curlmresult = curl_multi_setopt(m_curlm, CURLMOPT_PIPELINING, CURLPIPE_NOTHING);
		if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_setopt(CURLMOPT_PIPELINING) failed: ", curl_multi_strerror(curlmresult));

		// Create and initialize the curl easy interface object
		m_curl = curl_easy_init();
		if(m_curl == nullptr) throw string_exception(__func__, ": curl_easy_init() failed");

		try {

			// Set the options for the easy interface curl handle
			CURLcode curlresult = curl_easy_setopt(m_curl, CURLOPT_URL, url);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_ACCEPT_ENCODING, "identity, gzip, deflate");
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_FOLLOWLOCATION, 1L);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_MAXREDIRS, 5L);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT, 10L);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &xmlstream::curl_write);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_ERRORBUFFER, m_curlerr.get());

			if((curlresult == CURLE_OK) && (useragent != nullptr)) curlresult = curl_easy_setopt(m_curl, CURLOPT_USERAGENT, useragent);
			if((curlresult == CURLE_OK) && (share != nullptr)) curlresult = curl_easy_setopt(m_curl, CURLOPT_SHARE, share);

			if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed: ", curl_easy_strerror(curlresult));

			// Attempt to add the easy handle to the multi handle
			curlmresult = curl_multi_add_handle(m_curlm, m_curl);
			if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_add_handle() failed: ", curl_multi_strerror(curlmresult));

			try {

				// Attempt to begin the data transfer and wait for the initial chunk of data to become available
				transfer_until([&]() -> bool {

					available = (m_tail > m_head) ? (m_buffersize - m_tail) + m_head : m_head - m_tail;
					return (available > 0);
				});

				if(available == 0) throw string_exception(__func__, ": failed to receive HTTP response body");
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
// xmlstream Destructor

xmlstream::~xmlstream()
{
	close();
}

//---------------------------------------------------------------------------
// xmlstream::close
//
// Closes the stream
//
// Arguments:
//
//	NONE

void xmlstream::close(void)
{
	// Remove the easy handle from the multi handle and close them both out
	if((m_curlm != nullptr) && (m_curl != nullptr)) curl_multi_remove_handle(m_curlm, m_curl);
	if(m_curl != nullptr) curl_easy_cleanup(m_curl);
	if(m_curlm != nullptr) curl_multi_cleanup(m_curlm);

	m_curl = nullptr;				// Reset easy handle to null
	m_curlm = nullptr;				// Reset multi handle to null
}

//---------------------------------------------------------------------------
// xmlstream::create (static)
//
// Factory method, creates a new xmlstream instance
//
// Arguments:
//
//	url				- URL of the stream to be opened

std::unique_ptr<xmlstream> xmlstream::create(char const* url)
{
	return create(url, nullptr, nullptr);
}

//---------------------------------------------------------------------------
// xmlstream::create (static)
//
// Factory method, creates a new xmlstream instance
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	useragent		- User-Agent string to specify for the connection

std::unique_ptr<xmlstream> xmlstream::create(char const* url, char const* useragent)
{
	return create(url, useragent, nullptr);
}

//---------------------------------------------------------------------------
// xmlstream::create (static)
//
// Factory method, creates a new xmlstream instance
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	useragent		- User-Agent string to specify for the connection
//	share			- CURLSH instance to use for the connection

std::unique_ptr<xmlstream> xmlstream::create(char const* url, char const* useragent, CURLSH* share)
{
	return std::unique_ptr<xmlstream>(new xmlstream(url, useragent, share));
}

//---------------------------------------------------------------------------
// xmlstream::curl_write (static, private)
//
// libcurl callback to write transferred data into the buffer
//
// Arguments:
//
//	data		- Pointer to the data to be written
//	size		- Size of a single data element
//	count		- Number of data elements
//	context		- Caller-provided context pointer

size_t xmlstream::curl_write(void const* data, size_t size, size_t count, void* context)
{
	size_t				cb = size * count;			// Calculate the actual byte count
	size_t				byteswritten = 0;			// Total bytes actually written

	if((data == nullptr) || (cb == 0) || (context == nullptr)) return 0;

	// Cast the context pointer back into a xmlstream instance
	xmlstream* instance = reinterpret_cast<xmlstream*>(context);

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

	return byteswritten;
}

//---------------------------------------------------------------------------
// xmlstream::read
//
// Reads data from the live stream
//
// Arguments:
//
//	buffer		- Buffer to receive the live stream data
//	count		- Size of the destination buffer in bytes

size_t xmlstream::read(uint8_t* buffer, size_t count)
{
	size_t				bytesread = 0;			// Total bytes actually read
	size_t				available = 0;			// Available bytes to read

	assert((m_curlm != nullptr) && (m_curl != nullptr));

	if(count >= m_buffersize) throw std::invalid_argument("count");
	if(count == 0) return 0;

	// Transfer data into the ring buffer until data is available, the stream has completed, 
	// or an exception/error occurs
	transfer_until([&]() -> bool {

		available = (m_tail > m_head) ? (m_buffersize - m_tail) + m_head : m_head - m_tail;
		return (available > 0);
	});

	// If there is no available data in the ring buffer after transfer_until, indicate stream is finished
	if(available == 0) return 0;

	// Copy the data from the ring buffer into the destination buffer
	count = std::min(available, count); 
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

	return bytesread;
}

//---------------------------------------------------------------------------
// xmlstream::transfer_until (private)
//
// Executes the data transfer until the specified predicate has been satisfied
// or the transfer has completed
//
// Arguments:
//
//	predicate		- Predicate to be satisfied by the transfer

bool xmlstream::transfer_until(std::function<bool(void)> predicate)
{
	int				numfds;				// Number of active file descriptors

	assert((m_curlm != nullptr) && (m_curl != nullptr));

	// If the stream has been paused due to the ring buffer filling up, attempt to resume it
	if(m_paused) {

		// Determine the amount of free space in the ring buffer
		size_t free = (m_head < m_tail) ? m_tail - m_head : (m_buffersize - m_head) + m_tail;

		// If the buffer now has more than 50% free space available, resume the transfer.  Note that
		// calling curl_easy_pause() with CURLPAUSE_CONT synchronously attempts a write operation
		if(free >= (m_buffersize / 2)) {

			m_paused = false;									// Reset the stream paused flag
			curl_easy_pause(m_curl, CURLPAUSE_CONT);			// Resume transfer on the stream

			if(m_paused) return predicate();					// Still paused; abort
		}
	}

	// Attempt an initial data transfer operation and abort if there are no transfers
	CURLMcode curlmresult = curl_multi_perform(m_curlm, &numfds);
	if(numfds == 0) return predicate();

	// Continue to execute the data transfer until the predicate has been satisfied, the data
	// transfer operation is complete, or the stream has been paused due to a full buffer condition
	while((curlmresult == CURLM_OK) && (!m_paused) && (numfds > 0) && (predicate() == false)) {

		curlmresult = curl_multi_wait(m_curlm, nullptr, 0, 500, nullptr);
		if(curlmresult == CURLM_OK) curlmresult = curl_multi_perform(m_curlm, &numfds);
	}

	// If a curl error occurred, throw an exception
	if(curlmresult != CURLM_OK) throw string_exception(__func__, ": ", curl_multi_strerror(curlmresult));

	// If the number of file descriptors has reduced to zero, the transfer has completed.
	// Check for a cURL error or an HTTP error response on the transfer
	if(numfds == 0) {

		CURLcode		result = CURLE_OK;			// Assume everything went well
		long			responsecode = 200;			// Assume HTTP 200: OK

		// Get the cURL result code for the easy handle and remove it from the multi
		// interface to prevent any more data transfer operations from taking place
		curl_multi_get_result(m_curlm, m_curl, &result);
		curl_multi_remove_handle(m_curlm, m_curl);

		// If the cURL result indicated a failure, throw it as an exception
		if(result != CURLE_OK) throw string_exception((strlen(m_curlerr.get())) ? m_curlerr.get() : curl_easy_strerror(result));

		// The response code will come back as zero if there was no response from the host,
		// otherwise it should be a standard HTTP response code
		curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &responsecode);
		if(responsecode == 0) throw string_exception(__func__, ": no response from host");
		else if((responsecode < 200) || (responsecode > 299)) throw http_exception(responsecode);
	}

	return predicate();				// Re-evaluate the predicate as the result
}

//---------------------------------------------------------------------------

#pragma warning(pop)
