//---------------------------------------------------------------------------
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
//---------------------------------------------------------------------------

#ifndef __PVRTYPES_H_
#define __PVRTYPES_H_
#pragma once

#include <string>

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// CONSTANTS
//---------------------------------------------------------------------------

// MENUHOOK_XXXXXX
//
// Menu hook identifiers
static int const MENUHOOK_RECORD_DELETERERECORD					= 2;
static int const MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY		= 3;
static int const MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY		= 4;
static int const MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY		= 6;
static int const MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY	= 7;
static int const MENUHOOK_CHANNEL_DISABLE						= 9;
static int const MENUHOOK_CHANNEL_ADDFAVORITE					= 10;
static int const MENUHOOK_CHANNEL_REMOVEFAVORITE				= 11;
static int const MENUHOOK_SETTING_SHOWDEVICENAMES				= 12;
static int const MENUHOOK_SETTING_TRIGGERLISTINGDISCOVERY		= 13;
static int const MENUHOOK_SETTING_SHOWRECENTERRORS				= 14;
static int const MENUHOOK_SETTING_GENERATEDISCOVERYDIAGNOSTICS	= 15;

//---------------------------------------------------------------------------
// DATA TYPES
//---------------------------------------------------------------------------

// duplicate_prevention
//
// Defines the identifiers for series duplicate prevention values
enum duplicate_prevention {

	none		= 0,
	newonly		= 1,
	recentonly	= 2,
};

// timer_type
//
// Defines the identifiers for the various timer types (1-based)
enum timer_type {

	seriesrule			= 1,
	datetimeonlyrule	= 2,
	epgseriesrule		= 3,
	epgdatetimeonlyrule	= 4,
	seriestimer			= 5,
	datetimeonlytimer	= 6,
};

// tuning_protocol
//
// Defines the protocol to use when streaming directly from tuner(s)
enum tuning_protocol {

	http	= 0,
	rtpudp	= 1,
};

// settings
//
// Defines all of the configurable addon settings
struct settings {

	// pause_discovery_while_streaming
	//
	// Flag to pause the discovery activities while a live stream is active
	bool pause_discovery_while_streaming;

	// prepend_channel_numbers
	//
	// Flag to include the channel number in the channel name
	bool prepend_channel_numbers;

	// use_episode_number_as_title
	//
	// Flag to include the episode number in recording titles
	bool use_episode_number_as_title;

	// discover_recordings_after_playback
	//
	// Flag to re-discover recordings immediately after playback has stopped
	bool discover_recordings_after_playback;

	// use_backend_genre_strings
	//
	// Flag to use the backend provided genre strings instead of mapping them
	bool use_backend_genre_strings;

	// show_drm_protected_channels
	//
	// Flag indicating that DRM channels should be shown to the user
	bool show_drm_protected_channels;

	// channel_name_source
	//
	// Indicates the preferred source to use for channel naming purposes
	enum channel_name_source channel_name_source;

	// generate_epg_repeat_indicators
	//
	// Flag indicating that a repeat indicator should be appended to EPG episode names
	bool generate_epg_repeat_indicators;

	// disable_recording_categories
	//
	// Flag indicating that the category of a recording should be ignored
	bool disable_recording_categories;

	// generate_repeat_indicators
	//
	// Flag indicating that a repeat indicator should be appended to Recorded TV episode names
	bool generate_repeat_indicators;

	// use_airdate_as_recordingdate
	//
	// Flag indicating that the original air date should be reported as the recording date
	bool use_airdate_as_recordingdate;

	// use_actual_timer_times
	//
	// Flag indicating that the actual start and end times for timers should be reported
	bool use_actual_timer_times;

	// disable_backend_channel_logos
	//
	// Flag indicating that the channel logo thumbnail URLs should not be reported
	bool disable_backend_channel_logos;

	// delete_datetime_rules_after
	//
	// Amount of time (seconds) after which an expired date/time rule is deleted
	int delete_datetime_rules_after;

	// discover_devices_interval
	//
	// Interval at which the local network device discovery will occur (seconds)
	int discover_devices_interval;

	// discover_episodes_interval
	//
	// Interval at which the recording rule episodes discovery will occur (seconds)
	int discover_episodes_interval;

	// discover_lineups_interval
	//
	// Interval at which the local tuner device lineup discovery will occur (seconds)
	int discover_lineups_interval;

	// discover_recordings_interval
	//
	// Interval at which the local storage device recording discovery will occur (seconds)
	int discover_recordings_interval;

	// discover_recordingrules_interval
	//
	// Interval at which the recording rule discovery will occur (seconds)
	int discover_recordingrules_interval;

	// use_http_device_discovery
	//
	// Flag to discover devices via HTTP instead of local network broadcast
	bool use_http_device_discovery;

	// use_proxy_server
	//
	// Flag indicating that a proxy server should be used
	bool use_proxy_server;

	// proxy_server_address
	//
	// Indicates the hostname and port of the proxy server
	std::string proxy_server_address;

	// proxy_server_username
	//
	// Indicates the username required for the proxy server
	std::string proxy_server_username;

	// proxy_server_password
	//
	// Indicates the password required for the proxy server
	std::string proxy_server_password;

	// use_direct_tuning
	//
	// Flag indicating that Live TV will be handled directly from the tuner(s)
	bool use_direct_tuning;

	// direct_tuning_protocol
	//
	// Indicates the preferred protocol to use when streaming directly from the tuner(s)
	enum tuning_protocol direct_tuning_protocol;

	// direct_tuning_allow_drm
	//
	// Indicates that requests to stream DRM channels should be allowed
	bool direct_tuning_allow_drm;

	// stream_read_chunk_size
	//
	// Indicates the minimum number of bytes to return from a stream read
	int stream_read_chunk_size;

	// deviceauth_stale_after
	//
	// Amount of time (seconds) after which an expired device authorization code is removed
	int deviceauth_stale_after;

	// enable_recording_edl
	//
	// Enables support recorded TV edit decision lists
	bool enable_recording_edl;

	// recording_edl_folder
	//
	// Folder containing the recorded TV edit decision list files
	std::string recording_edl_folder;

	// recording_edl_folder_2
	//
	// Additional folder containing the recorded TV edit decision list files
	std::string recording_edl_folder_2;

	// recording_edl_folder_3
	//
	// Additional folder containing the recorded TV edit decision list files
	std::string recording_edl_folder_3;

	// recording_edl_folder_is_flat
	//
	// Indicates that the specified EDL folder is flattened (no subdirectories)
	bool recording_edl_folder_is_flat;

	// recording_edl_cut_as_comskip
	//
	// Indicates that EDL CUT indicators should be replaced with COMSKIP indicators
	bool recording_edl_cut_as_comskip;

	// recording_edl_start_padding
	//
	// Indicates the number of milliseconds to add to an EDL start value
	int recording_edl_start_padding;

	// recording_edl_end_padding
	//
	// Indicates the number of milliseconds to subtract to an EDL end value
	int recording_edl_end_padding;

	// enable_radio_channel_mapping
	//
	// Flag indicating that a radio channel mapping should be used
	bool enable_radio_channel_mapping;

	// radio_channel_mapping_file
	//
	// Path to the radio channel mapping file, if present
	std::string radio_channel_mapping_file;

	// block_radio_channel_video_streams
	//
	// Flag to suppress the video streams for mapped radio channels
	bool block_radio_channel_video_streams;
};

//---------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __PVRTYPES_H_
