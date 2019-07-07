//---------------------------------------------------------------------------
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
//---------------------------------------------------------------------------

#include "stdafx.h"
#include "devicestream.h"

#include "string_exception.h"

#pragma warning(push, 4)

// devicestream::DEFAULT_MEDIA_TYPE (static)
//
// Default media type to report for the stream
char const* devicestream::DEFAULT_MEDIA_TYPE = "video/mp2t";

// devicestream::MAXIMUM_WAIT_TIME (static)
//
// The maximum amount of time to wait for stream data
uint64_t const devicestream::MAXIMUM_WAIT_TIME = 1000;

// devicestream::WAIT_INTERVAL (static)
//
// The amount of time to wait at once for stream data
uint64_t const devicestream::WAIT_INTERVAL = 15;

//---------------------------------------------------------------------------
// devicestream Constructor (private)
//
// Arguments:
//
//	selector	- HDHomeRun device selector instance
//	device		- Selected HDHomeRun device instance

devicestream::devicestream(struct hdhomerun_device_selector_t* selector, struct hdhomerun_device_t* device) : 
	m_selector(selector), m_device(device)
{
	assert((selector != nullptr) && (device != nullptr));
	if(selector == nullptr) throw std::invalid_argument("selector");
	if(device == nullptr) throw std::invalid_argument("device");

	// Attempt to start the device stream, the tuning parameters should have been set by create()
	int result = hdhomerun_device_stream_start(device);
	if(result != 1) { throw string_exception(__func__, ": failed to start device stream"); }
}

//---------------------------------------------------------------------------
// devicestream Destructor

devicestream::~devicestream()
{
	close();

	// Release the HDHomeRun device selector and the contained device objects
	if(m_selector != nullptr) hdhomerun_device_selector_destroy(m_selector, true);
}

//---------------------------------------------------------------------------
// devicestream::canseek
//
// Gets a flag indicating if the stream allows seek operations
//
// Arguments:
//
//	NONE

bool devicestream::canseek(void) const
{
	return false;
}

//---------------------------------------------------------------------------
// devicestream::close
//
// Closes the stream
//
// Arguments:
//
//	NONE

void devicestream::close(void)
{
	if(m_device) {

		// Stop the stream from the device and release the lockkey being held
		hdhomerun_device_stream_stop(m_device);
		hdhomerun_device_tuner_lockkey_release(m_device);

		m_device = nullptr;			// Device is no longer active
	}
}

//---------------------------------------------------------------------------
// devicestream::currentpts
//
// Gets the current presentation timestamp value
//
// Arguments:
//
//	NONE

uint64_t devicestream::currentpts(void) const
{
	return 0;
}

//---------------------------------------------------------------------------
// devicestream::currenttime
//
// Gets the current playback time based on the presentation timestamps
//
// Arguments:
//
//	NONE

time_t devicestream::currenttime(void) const
{
	return 0;
}

//---------------------------------------------------------------------------
// devicestream::create (static)
//
// Factory method, creates a new devicestream instance
//
// Arguments:
//
//	devices		- vector<> of valid devices for the target stream
//	vchannel	- Virtual channel number of the stream to create

std::unique_ptr<devicestream> devicestream::create(std::vector<std::string> const& devices, char const* vchannel)
{
	assert(vchannel != nullptr);
	if((vchannel == nullptr) || (*vchannel == '\0')) throw std::invalid_argument("vchannel");

	// Allocate and initialize the device selector
	struct hdhomerun_device_selector_t* selector = hdhomerun_device_selector_create(nullptr);
	if(selector == nullptr) throw string_exception(__func__, ": hdhomerun_device_selector_create() failed");

	try {

		// Add each of the possible device/tuner combinations to the selector
		for(auto const& iterator : devices) {

			struct hdhomerun_device_t* device = hdhomerun_device_create_from_str(iterator.c_str(), nullptr);
			if(device == nullptr) throw string_exception(__func__, ": hdhomerun_device_create_from_str() failed");

			hdhomerun_device_selector_add_device(selector, device);
		}

		// Let libhdhomerun select and lock a device for us from the available possibilities
		struct hdhomerun_device_t* selected = hdhomerun_device_selector_choose_and_lock(selector, nullptr);
		if(selected == nullptr) throw string_exception(__func__, ": no devices are available to create the requested stream");

		try { 
			
			// Attempt to set the virtual channel for the selected tuner
			int result = hdhomerun_device_set_tuner_vchannel(selected, vchannel);
			if(result != 1) { throw string_exception(__func__, ": unable to set virtual channel ", vchannel, " on device"); }

			// Create the device stream for the selected tuner
			return std::unique_ptr<devicestream>(new devicestream(selector, selected)); 
		
		}
		catch(...) { hdhomerun_device_tuner_lockkey_release(selected); throw; }
	}

	catch(...) { hdhomerun_device_selector_destroy(selector, true); throw; }
}

//---------------------------------------------------------------------------
// devicestream::length
//
// Gets the length of the stream; or -1 if stream is real-time
//
// Arguments:
//
//	NONE

long long devicestream::length(void) const
{
	return -1;
}

//---------------------------------------------------------------------------
// devicestream::mediatype
//
// Gets the media type of the stream
//
// Arguments:
//
//	NONE

char const* devicestream::mediatype(void) const
{
	return DEFAULT_MEDIA_TYPE;
}

//---------------------------------------------------------------------------
// devicestream::position
//
// Gets the current position of the stream
//
// Arguments:
//
//	NONE

long long devicestream::position(void) const
{
	return -1;
}

//---------------------------------------------------------------------------
// devicestream::read
//
// Reads data from the live stream
//
// Arguments:
//
//	buffer		- Buffer to receive the live stream data
//	count		- Size of the destination buffer in bytes

size_t devicestream::read(uint8_t* buffer, size_t count)
{
	uint8_t*		streambuffer = nullptr;		// Pointer the stream data
	size_t			buffersize = 0;				// Bytes available in the buffer
	uint64_t		waited = 0;					// Amount of time spent waiting for data

	assert(m_device != nullptr);
	if(m_device == nullptr) throw string_exception(__func__, ": stream has been closed");

	// There isn't always data available in the buffer, sleep in WAIT_INTERVAL chunks for
	// more data to become ready to transfer into the output buffer
	while((streambuffer == nullptr) && (waited < MAXIMUM_WAIT_TIME)) {

		streambuffer = hdhomerun_device_stream_recv(m_device, count, &buffersize);
		if(streambuffer == nullptr) { msleep_approx(WAIT_INTERVAL); waited += WAIT_INTERVAL; }
	}

	// If data is available, copy it into the output buffer; also set the stream start time
	// to the time when the first chunk of data became available on the stream
	if((streambuffer != nullptr) && (buffersize > 0)) {

		memcpy(buffer, streambuffer, buffersize);
		if(m_starttime == 0) m_starttime = time(nullptr);
	}

	return buffersize;
}

//---------------------------------------------------------------------------
// devicestream::realtime
//
// Gets a flag indicating if the stream is real-time
//
// Arguments:
//
//	NONE

bool devicestream::realtime(void) const
{
	return true;
}

//---------------------------------------------------------------------------
// devicestream::seek
//
// Sets the stream pointer to a specific position
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

long long devicestream::seek(long long /*position*/, int /*whence*/)
{
	return -1;
}

//---------------------------------------------------------------------------
// devicestream::startpts
//
// Gets the initial presentation timestamp value
//
// Arguments:
//
//	NONE

uint64_t devicestream::startpts(void) const
{
	return 0;
}

//---------------------------------------------------------------------------
// devicestream::starttime
//
// Gets the time at which the stream started
//
// Arguments:
//
//	NONE

time_t devicestream::starttime(void) const
{
	return m_starttime;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
