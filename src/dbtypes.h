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

#ifndef __DBTYPES_H_
#define __DBTYPES_H_
#pragma once

#include <stdint.h>
#include <time.h>

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// CONSTANTS
//---------------------------------------------------------------------------

// DATABASE_CONNECTIONPOOL_SIZE
//
// Specifies the default size of the database connection pool
static size_t const DATABASE_CONNECTIONPOOL_SIZE = 5;

// DATABASE_SCHEMA_VERSION
//
// This value needs to be incremented with any database schema change
static char const DATABASE_SCHEMA_VERSION[] = "13";

//---------------------------------------------------------------------------
// DATA TYPES
//---------------------------------------------------------------------------

// channelid
//
// Unique identifier for a channel
union channelid {

	struct {
	
		// CCCCCCCCCCCCCCCCCCCC SSSSSSSSSSSS (little endian)
		//
		unsigned int	subchannel : 12;	// Subchannel number (0 - 4095)
		unsigned int	channel : 20;		// Channel number (0 - 1048575)

	} parts;
	
	unsigned int value;						// Complete channel id
};

static_assert(sizeof(channelid) == sizeof(uint32_t), "channelid structure must be same size as a uint32_t");

// channel
//
// Information about a single channel enumerated from the database
struct channel {

	union channelid		channelid;
	char const*			channelname;
	char const*			inputformat;
	char const*			iconurl;
	bool				drm;
};

// channel_name_source
//
// Defines the source to use when determining what name to give a channel
enum channel_name_source {

	xmltv				= 0,
	xmltvaltname		= 1,
	xmltvnetwork		= 2,
	device				= 3,
};

// channel_tuner
//
// Defines a tuner that is capable of streaming a channel
struct channel_tuner {

	char const*			tunerid;
	bool				islegacy;
	char const*			frequency;
	char const*			program;
};

// channel_visibility
//
// Flag indicating a channel's visibility
enum channel_visibility {

	enabled				= 0,
	favorite			= 1,
	disabled			= 2,
};

// device_name
//
// Information about a single device enumerated from the database
struct device_name {

	char const*			name;
};

// listing
//
// Information about a single listing enumerated from the database
struct listing {

	char const*			seriesid;
	char const*			title;
	unsigned int		broadcastid;
	unsigned int		channelid;
	int64_t				starttime;
	int64_t				endtime;
	char const*			synopsis;
	int					year;
	char const*			iconurl;
	char const*			programtype;
	int					genretype;
	char const*			genres;
	int64_t				originalairdate;
	int					seriesnumber;
	int					episodenumber;
	char const*			episodename;
	bool				isnew;
	int					starrating;
};

// recording
//
// Information about a single recording enumerated from the database
struct recording {

	char const*			recordingid;
	char const*			title;
	char const*			episodename;
	int					firstairing;
	int64_t				originalairdate;
	char const*			programtype;
	int					seriesnumber;
	int					episodenumber;
	int					year;
	char const*			streamurl;
	char const*			directory;
	char const*			plot;
	char const*			channelname;
	char const*			iconpath;
	char const*			thumbnailpath;
	int64_t				recordingtime;
	int					duration;
	uint32_t			lastposition;
	union channelid		channelid;
	char const*			category;
};

// recordingrule_type
//
// Type of an existing recording rule
enum recordingrule_type {

	series						= 0,
	datetimeonly				= 1,
};

// recordingrule
//
// Information about a backend recording rule
struct recordingrule {

	unsigned int				recordingruleid;
	enum recordingrule_type		type;
	char const*					seriesid;
	union channelid				channelid;
	bool						recentonly;
	int64_t						afteroriginalairdateonly;
	int64_t						datetimeonly;
	char const*					title;
	char const*					synopsis;
	unsigned int				startpadding;
	unsigned int				endpadding;
};

// series
//
// Information about a series
struct series {

	char const*					title;
	char const*					seriesid;
};

// storage_space
//
// Information about the available storage space
struct storage_space {

	long long					total;
	long long					available;
};

// timer
//
// Information about a timer
struct timer {

	unsigned int				recordingruleid;
	enum recordingrule_type		parenttype;
	unsigned int				timerid;
	char const*					seriesid;
	union channelid				channelid;
	int64_t						starttime;
	int64_t						endtime;
	char const*					title;
	char const*					synopsis;
	unsigned int				startpadding;
	unsigned int				endpadding;
};

// xmltv_channel
//
// Information about a channel enumerated by the xmltv virtual table
struct xmltv_channel
{
	char const*					id;
	char const*					number;
	char const*					name;
	char const*					altname;
	char const*					network;
	char const*					iconsrc;
};

// xmltv_onchannel_callback
//
// Callback function passed as a pointer to the xmltv virtual table
using xmltv_onchannel_callback = std::function<void(struct xmltv_channel const& channel)>;

//---------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __DBTYPES_H_
