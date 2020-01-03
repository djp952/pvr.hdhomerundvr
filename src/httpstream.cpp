//---------------------------------------------------------------------------
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
//---------------------------------------------------------------------------

#include "stdafx.h"
#include "httpstream.h"

#include <algorithm>
#include <assert.h>
#include <string.h>

#include "align.h"
#include "http_exception.h"
#include "string_exception.h"

#pragma warning(push, 4)

// httpstream::DEFAULT_CHUNK_SIZE (static)
//
// Default stream chunk size
size_t const httpstream::DEFAULT_CHUNK_SIZE = (4 KiB);

// httpstream::DEFAULT_MEDIA_TYPE (static)
//
// Default media type to report for the stream
char const* httpstream::DEFAULT_MEDIA_TYPE = "video/mp2t";

// httpstream::DEFAULT_RINGBUFFER_SIZE (static)
//
// Default ring buffer size, in bytes
size_t const httpstream::DEFAULT_RINGBUFFER_SIZE = (1 MiB);

// httpstream::MAX_STREAM_LENGTH (static)
//
// Maximum allowable stream length; indicates a real-time stream
long long const httpstream::MAX_STREAM_LENGTH = std::numeric_limits<long long>::max();

// httpstream::MPEGTS_PACKET_LENGTH (static)
//
// Length of a single mpeg-ts data packet
size_t const httpstream::MPEGTS_PACKET_LENGTH = 188;
	
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
// httpstream Constructor (private)
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	buffersize		- Ring buffer size, in bytes
//	chunksize		- Chunk size to use for the stream

httpstream::httpstream(char const* url, size_t buffersize, size_t chunksize) :
	m_chunksize(std::max(align::down(chunksize, MPEGTS_PACKET_LENGTH), MPEGTS_PACKET_LENGTH)),
	m_buffersize(align::up(buffersize, 65536))
{
	size_t		available = 0;				// Amount of available ring buffer data

	if(url == nullptr) throw std::invalid_argument("url");

	// Allocate the ring buffer using the 64KiB upward-aligned buffer size
	m_buffer = std::unique_ptr<uint8_t[]>(new uint8_t[m_buffersize]);
	if(!m_buffer) throw std::bad_alloc();

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
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_TIME, 5L);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERFUNCTION, &httpstream::curl_responseheaders);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, this);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &httpstream::curl_write);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
			if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_RANGE, "0-");
			if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed: ", curl_easy_strerror(curlresult));

			// Attempt to add the easy handle to the multi handle
			curlmresult = curl_multi_add_handle(m_curlm, m_curl);
			if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_add_handle() failed: ", curl_multi_strerror(curlmresult));

			try {

				// Attempt to begin the data transfer and wait for both the HTTP headers to be processed
				// and for the initial chunk of data to become available in the ring buffer
				transfer_until([&]() -> bool { 
					
					available = (m_tail > m_head) ? (m_buffersize - m_tail) + m_head : m_head - m_tail;
					return ((m_headers == true) && (available > 0));
				});

				if(!m_headers) throw string_exception(__func__, ": failed to receive HTTP response headers");
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
// httpstream Destructor

httpstream::~httpstream()
{
	close();
}

//---------------------------------------------------------------------------
// httpstream::canseek
//
// Gets a flag indicating if the stream allows seek operations
//
// Arguments:
//
//	NONE

bool httpstream::canseek(void) const
{
	return m_canseek;
}

//---------------------------------------------------------------------------
// httpstream::chunksize
//
// Gets the stream chunk size
//
// Arguments:
//
//	NONE

size_t httpstream::chunksize(void) const
{
	return m_chunksize;
}

//---------------------------------------------------------------------------
// httpstream::close
//
// Closes the stream
//
// Arguments:
//
//	NONE

void httpstream::close(void)
{
	// Remove the easy handle from the multi handle and close them both out
	if((m_curlm != nullptr) && (m_curl != nullptr)) curl_multi_remove_handle(m_curlm, m_curl);
	if(m_curl != nullptr) curl_easy_cleanup(m_curl);
	if(m_curlm != nullptr) curl_multi_cleanup(m_curlm);

	m_curl = nullptr;				// Reset easy handle to null
	m_curlm = nullptr;				// Reset multi handle to null
}

//---------------------------------------------------------------------------
// httpstream::create (static)
//
// Factory method, creates a new httpstream instance
//
// Arguments:
//
//	url				- URL of the stream to be opened

std::unique_ptr<httpstream> httpstream::create(char const* url)
{
	return create(url, DEFAULT_RINGBUFFER_SIZE, DEFAULT_CHUNK_SIZE);
}

//---------------------------------------------------------------------------
// httpstream::create (static)
//
// Factory method, creates a new httpstream instance
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	buffersize		- Ring buffer size, in bytes

std::unique_ptr<httpstream> httpstream::create(char const* url, size_t buffersize)
{
	return create(url, buffersize, DEFAULT_CHUNK_SIZE);
}

//---------------------------------------------------------------------------
// httpstream::create (static)
//
// Factory method, creates a new httpstream instance
//
// Arguments:
//
//	url				- URL of the stream to be opened
//	buffersize		- Ring buffer size, in bytes
//	readmincount	- Minimum bytes to return from a read operation

std::unique_ptr<httpstream> httpstream::create(char const* url, size_t buffersize, size_t readmincount)
{
	return std::unique_ptr<httpstream>(new httpstream(url, buffersize, readmincount));
}

//---------------------------------------------------------------------------
// httpstream::curl_responseheaders (static, private)
//
// libcurl callback to process response headers
//
// Arguments:
//
//	data		- Pointer to the response header data
//	size		- Size of a single data element
//	count		- Number of data elements
//	context		- Caller-provided context pointer

size_t httpstream::curl_responseheaders(char const* data, size_t size, size_t count, void* context)
{
	static const char ACCEPT_RANGES_HEADER[]		= "Accept-Ranges: bytes";
	static const char CONTENT_RANGE_HEADER[]		= "Content-Range: bytes";
	static const char CONTENT_TYPE_HEADER[]			= "Content-Type:";
	static const char EMPTY_HEADER[]				= "\r\n";
	static const size_t ACCEPT_RANGES_HEADER_LEN	= strlen(ACCEPT_RANGES_HEADER);
	static const size_t CONTENT_RANGE_HEADER_LEN	= strlen(CONTENT_RANGE_HEADER);
	static const size_t CONTENT_TYPE_HEADER_LEN		= strlen(CONTENT_TYPE_HEADER);
	static const size_t EMPTY_HEADER_LEN			= strlen(EMPTY_HEADER);

	size_t cb = size * count;						// Calculate the actual byte count
	if(cb == 0) return 0;							// Nothing to do

	// Cast the context pointer back into a httpstream instance
	httpstream* instance = reinterpret_cast<httpstream*>(context);

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

		// Attempt to parse a complete Content-Range: header to retrieve all the values, otherwise
		// fall back on just attempting to get the size.  The latter condition occurs on a seek
		// beyond the size of a fixed-length stream, so set the start value to match the size
		int result = sscanf(data, "Content-Range: bytes %lld-%lld/%lld", &start, &end, &length);
		if((result == 0) && (sscanf(data, "Content-Range: bytes */%lld", &length)) == 1) start = length;

		// Reset the stream read/write positions and overall length
		instance->m_startpos = instance->m_readpos = instance->m_writepos = start;
		instance->m_length = length;
	}

	// Content-Type: <media-type>[; charset=<charset>][ ;boundary=<boundary>]
	else if((cb >= CONTENT_TYPE_HEADER_LEN) && (strncmp(CONTENT_TYPE_HEADER, data, CONTENT_TYPE_HEADER_LEN) == 0)) {

		char mediatype[128];						// <media-type>

		// Attempt to parse the media-type from the Context-Type header and set for the stream if found
		if(sscanf(std::string(data, cb).c_str(), "Content-Type: %127[^;\r\n]", mediatype) == 1) 
			instance->m_mediatype.assign(mediatype);
	}

	// \r\n (empty header)
	else if((cb >= EMPTY_HEADER_LEN) && (strncmp(EMPTY_HEADER, data, EMPTY_HEADER_LEN) == 0)) {

		// The final header has been processed, indicate that by setting the flag
		instance->m_headers = true;
	}

	return cb;
}

//---------------------------------------------------------------------------
// httpstream::curl_write (static, private)
//
// libcurl callback to write transferred data into the buffer
//
// Arguments:
//
//	data		- Pointer to the data to be written
//	size		- Size of a single data element
//	count		- Number of data elements
//	context		- Caller-provided context pointer

size_t httpstream::curl_write(void const* data, size_t size, size_t count, void* context)
{
	size_t				cb = size * count;			// Calculate the actual byte count
	size_t				byteswritten = 0;			// Total bytes actually written

	if((data == nullptr) || (cb == 0) || (context == nullptr)) return 0;

	// Cast the context pointer back into a httpstream instance
	httpstream* instance = reinterpret_cast<httpstream*>(context);

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
// httpstream::length
//
// Gets the length of the stream; or -1 if stream is real-time
//
// Arguments:
//
//	NONE

long long httpstream::length(void) const
{
	return (m_length == MAX_STREAM_LENGTH) ? -1 : m_length;
}

//---------------------------------------------------------------------------
// httpstream::mediatype
//
// Gets the media type of the stream
//
// Arguments:
//
//	NONE

char const* httpstream::mediatype(void) const
{
	return m_mediatype.c_str();
}

//---------------------------------------------------------------------------
// httpstream::position
//
// Gets the current position of the stream
//
// Arguments:
//
//	NONE

long long httpstream::position(void) const
{
	return m_readpos;
}

//---------------------------------------------------------------------------
// httpstream::read
//
// Reads data from the live stream
//
// Arguments:
//
//	buffer		- Buffer to receive the live stream data
//	count		- Size of the destination buffer in bytes

size_t httpstream::read(uint8_t* buffer, size_t count)
{
	size_t				bytesread = 0;			// Total bytes actually read
	size_t				available = 0;			// Available bytes to read

	assert((m_curlm != nullptr) && (m_curl != nullptr));

	if(count >= m_buffersize) throw std::invalid_argument("count");

	// The count should be aligned down to MPEGTS_PACKET_LENGTH, even though the chunk
	// size is reported, the application won't obey that value unless the stream is seekable
	count = align::down(count, MPEGTS_PACKET_LENGTH);
	if(count == 0) return 0;

	// Transfer data into the ring buffer until the minimum amount of data is available, 
	// the stream has completed, or an exception/error occurs
	transfer_until([&]() -> bool {

		available = (m_tail > m_head) ? (m_buffersize - m_tail) + m_head : m_head - m_tail;
		return (available >= count);
	});

	// If there is no available data in the ring buffer after transfer_until, indicate stream is finished
	if(available == 0) return 0;

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

	return bytesread;
}

//---------------------------------------------------------------------------
// httpstream::realtime
//
// Gets a flag indicating if the stream is real-time
//
// Arguments:
//
//	NONE

bool httpstream::realtime(void) const
{
	return (m_length == MAX_STREAM_LENGTH);
}

//---------------------------------------------------------------------------
// httpstream::restart (private)
//
// Restarts the stream at the specified position
//
// Arguments:
//
//	position		- Requested starting position for the transfer

long long httpstream::restart(long long position)
{
	size_t		available = 0;				// Amount of available ring buffer data

	assert((m_curlm != nullptr) && (m_curl != nullptr));
	assert(position >= 0);

	// Remove the easy transfer handle from the multi transfer handle
	CURLMcode curlmresult = curl_multi_remove_handle(m_curlm, m_curl);
	if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_remove_handle() failed: ", curl_multi_strerror(curlmresult));

	// Reset all of the stream state and ring buffer values back to the defaults; leave the 
	// start time and start presentation timestamp values at their original values
	m_paused = m_headers = m_canseek = false;
	m_head = m_tail = 0;
	m_startpos = m_readpos = m_writepos = 0;
	m_length = MAX_STREAM_LENGTH;

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

	// Attempt to begin the data transfer and wait for both the HTTP headers to be processed
	// and for the initial chunk of data to become available in the ring buffer
	transfer_until([&]() -> bool { 
					
		available = (m_tail > m_head) ? (m_buffersize - m_tail) + m_head : m_head - m_tail;
		return ((m_headers == true) && (available > 0));
	});

	if(!m_headers) throw string_exception(__func__, ": failed to receive HTTP response headers");
	if(available == 0) throw string_exception(__func__, ": failed to receive HTTP response body");

	return m_readpos;					// Return new starting position of the stream
}

//---------------------------------------------------------------------------
// httpstream::seek
//
// Sets the stream pointer to a specific position
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

long long httpstream::seek(long long position, int whence)
{
	long long			newposition = 0;			// New stream position

	assert((m_curlm != nullptr) && (m_curl != nullptr));

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
// httpstream::transfer_until (private)
//
// Executes the data transfer until the specified predicate has been satisfied
// or the transfer has completed
//
// Arguments:
//
//	predicate		- Predicate to be satisfied by the transfer

bool httpstream::transfer_until(std::function<bool(void)> predicate)
{
	int				numfds;				// Number of active file descriptors

	assert((m_curlm != nullptr) && (m_curl != nullptr));

	// If the stream has been paused due to the ring buffer filling up, attempt to resume it
	// CAUTION: calling curl_easy_pause() with CURLPAUSE_CONT *immediately* attempts to write 
	// outstanding data into the ring buffer so when it returns m_paused may have been set 
	// back to true if the ring buffer is still full after attempting to resume
	if(m_paused) {

		m_paused = false;									// Reset the stream paused flag
		curl_easy_pause(m_curl, CURLPAUSE_CONT);			// Resume transfer on the stream

		if(m_paused) return predicate();					// Still paused, abort
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
		if(result != CURLE_OK) throw string_exception(curl_easy_strerror(result));

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
