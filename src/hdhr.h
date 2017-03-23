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

#ifndef __HDHR_H_
#define __HDHR_H_
#pragma once

#include <functional>
#include <string>
#include <vector>

#pragma warning(push, 4)				// Enable maximum compiler warnings

//---------------------------------------------------------------------------
// DATA TYPES
//---------------------------------------------------------------------------

// device_type
//
// Type of a discovered HDHomeRun device
enum device_type {

	tuner				= 0,
	storage				= 1,
};

// discover_device
//
// Information about a single HDHomeRun device discovered via broadcast
struct discover_device {

	enum device_type	devicetype;
	uint32_t			deviceid;
	char const*			baseurl;
};

// enumerate_devices_callback
//
// Callback function passed to enumerate_devices
using enumerate_devices_callback = std::function<void(struct discover_device const& device)>;

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

// enumerate_devices
//
// Enumerates all of the HDHomeRun devices discovered via broadcast (libhdhomerun)
void enumerate_devices(enumerate_devices_callback callback);

// select_tuner
//
// Selects an available tuner device from a list of possibilities
std::string select_tuner(std::vector<std::string> const& possibilities);

//---------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __HDHR_H_
