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

#include <hdhomerun.h>
#include <memory>
#include <string>
#include <vector>

#include "hdhr.h"
#include "string_exception.h"

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// enumerate_devices
//
// Enumerates all of the HDHomeRun devices discovered via broadcast (libhdhomerun)
//
// Arguments:
//
//	callback	- Callback to be invoked for each discovered device

void enumerate_devices(enumerate_devices_callback callback)
{
	// Allocate enough heap storage to hold up to 64 enumerated devices on the network
	std::unique_ptr<struct hdhomerun_discover_device_t[]> devices(new struct hdhomerun_discover_device_t[64]);

	// Use the libhdhomerun broadcast discovery mechanism to find all devices on the local network
	int result = hdhomerun_discover_find_devices_custom_v2(0, HDHOMERUN_DEVICE_TYPE_WILDCARD,
		HDHOMERUN_DEVICE_ID_WILDCARD, &devices[0], 64);
	if(result == -1) throw string_exception(__func__, ": hdhomerun_discover_find_devices_custom_v2 failed");

	for(int index = 0; index < result; index++) {

		// Only tuner and storage devices are supported
		if((devices[index].device_type != HDHOMERUN_DEVICE_TYPE_TUNER) && (devices[index].device_type != HDHOMERUN_DEVICE_TYPE_STORAGE)) continue;

		// Only non-legacy devices are supported
		if(devices[index].is_legacy) continue;

		// Only devices with a base URL string are supported
		if(strlen(devices[index].base_url) == 0) continue;

		// Convert the hdhomerun_discover_device_t structure into a discover_device for the caller
		struct discover_device device;
		device.devicetype = (devices[index].device_type == HDHOMERUN_DEVICE_TYPE_STORAGE) ? device_type::storage : device_type::tuner;
		device.deviceid = devices[index].device_id;
		device.baseurl = devices[index].base_url;

		callback(device);
	}
}

//---------------------------------------------------------------------------
// select_tuner
//
// Selects an available tuner device from a list of possibilities
//
// Arguments:
//
//	possibilities	- vector<> of tuners to select from

std::string select_tuner(std::vector<std::string> const& possibilities)
{
	std::string					tunerid;			// Selected tuner identifier

	// Allocate and initialize the device selector
	struct hdhomerun_device_selector_t* selector = hdhomerun_device_selector_create(nullptr);
	if(selector == nullptr) throw string_exception("hdhomerun_device_selector_create() failed");

	try {

		// Add each of the possible device/tuner combinations to the selector
		for(auto const& iterator : possibilities) {

			struct hdhomerun_device_t* device = hdhomerun_device_create_from_str(iterator.c_str(), nullptr);
			if(device == nullptr) throw string_exception("hdhomerun_device_create_from_str() failed");

			hdhomerun_device_selector_add_device(selector, device);
		}

		// NOTE: There is an inherent race condition here with the tuner lock implementation.  When the tuner
		// is selected here it will be locked, but it cannot remain locked since the ultimate purpose here is
		// to generate an HTTP URL for the application to use.  The HTTP stream will attempt it's own lock
		// and would fail if left locked after this function completes.  No way to tell it to use an existing lock.

		// Let libhdhomerun pick a free tuner for us from the available possibilities
		struct hdhomerun_device_t* selected = hdhomerun_device_selector_choose_and_lock(selector, nullptr);
		if(selected) {

			tunerid = hdhomerun_device_get_name(selected);			// DDDDDDDD-T; D=DeviceID, T=TunerID
			hdhomerun_device_tuner_lockkey_release(selected);		// Release the acquired lock
		}

		// Release the selector along with all of the generated device structures
		hdhomerun_device_selector_destroy(selector, true);
	}

	catch(...) { hdhomerun_device_selector_destroy(selector, true); }

	return tunerid;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
