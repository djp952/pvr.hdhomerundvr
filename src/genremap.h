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

#ifndef __GENREMAP_H_
#define __GENREMAP_H_
#pragma once

#include <xbmc_pvr_types.h>

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// DATA TYPES
//---------------------------------------------------------------------------

// genremap_element
//
// Defines a single entry in the genre mapping table
struct genremap_element {

	char const*		genre;				// Genre/category string
	int				genretype;			// DVB-SI EIT content descriptor code
};

//---------------------------------------------------------------------------
// GENRE MAPPING TABLE
//---------------------------------------------------------------------------

// GENRE_MAPPING_TABLE
//
// Static table used to (re)generate the contents of the genremap database table
//
// Only map genres that will not correspond to the default mapping that will be 
// set via the TMS program type:
//
// PROGRAM TYPE  DEFAULT MAPPING
// ------------  ---------------
//      EP       EPG_EVENT_CONTENTMASK_SHOW
//      MV       EPG_EVENT_CONTENTMASK_MOVIEDRAMA
//      SH       EPG_EVENT_CONTENTMASK_SHOW
//      SP       EPG_EVENT_CONTENTMASK_SPORTS


static genremap_element const GENRE_MAPPING_TABLE[] = {

	{ "Action sports",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Aerobics",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Archery",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Arm wrestling",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Art",						EPG_EVENT_CONTENTMASK_ARTSCULTURE },
	{ "Arts/crafts",				EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Auto",						EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Auto racing",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Aviation",					EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Badminton",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Ballet",						EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Baseball",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Basketball",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Beach soccer",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Beach volleyball",			EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Biathlon",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Bicycle",					EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Bicycle racing",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Billiards",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Blackjack",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Boat",						EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Boat racing",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Bobsled",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Bodybuilding",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Bowling",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Boxing",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Bullfighting",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Bus./financial",				EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS },
	{ "Canoe",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Card games",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Cheerleading",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Children",					EPG_EVENT_CONTENTMASK_CHILDRENYOUTH },
	{ "Children-special",			EPG_EVENT_CONTENTMASK_CHILDRENYOUTH },
	{ "Children-talk",				EPG_EVENT_CONTENTMASK_CHILDRENYOUTH },
	{ "Children-music",				EPG_EVENT_CONTENTMASK_CHILDRENYOUTH },
	{ "Collectibles",				EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Computers",					EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE },
	{ "Consumer",					EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS },
	{ "Cooking",					EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Cricket",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Curling",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Dance",						EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Darts",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Debate",						EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS },
	{ "Diving",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Documentary",				EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS },
	{ "Dog racing",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Dog sled",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Drag racing",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Educational",				EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE },
	{ "Environment",				EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE },
	{ "Equestrian",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Exercise",					EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Extreme",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Fashion",					EPG_EVENT_CONTENTMASK_ARTSCULTURE },
	{ "Fencing",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Field hockey",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Figure skating",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Fishing",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Football",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Gaelic football",			EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Golf",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Gymnastics",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Handball",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Health",						EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Hockey",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Holiday-children",			EPG_EVENT_CONTENTMASK_CHILDRENYOUTH },
	{ "Holiday-children special",	EPG_EVENT_CONTENTMASK_CHILDRENYOUTH },
	{ "Holiday music",				EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Holiday music special",		EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Home improvement",			EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Horse ",						EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "House/garden",				EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "How-to",						EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Hunting",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Hurling",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Hydroplane racing",			EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Indoor soccer",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Interview",					EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS },
	{ "Intl basketball",			EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Intl hockey",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Intl soccer",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Kayaking",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Lacrosse",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Luge",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Martial arts",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Medical",					EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE },
	{ "Mixed martial arts",			EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Motorsports",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Motorcycle",					EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Motorcycle racing",			EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Mountain biking",			EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Music",						EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Music special",				EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Music talk",					EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Musical",					EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Musical comedy",				EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Nature",						EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE },
	{ "News",						EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS },
	{ "Newsmagazine",				EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS },
	{ "Olympics",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Opera",						EPG_EVENT_CONTENTMASK_MUSICBALLETDANCE },
	{ "Outdoors",					EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Pelota vasca",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Performing arts",			EPG_EVENT_CONTENTMASK_ARTSCULTURE },
	{ "Playoff sports",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Politics",					EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS },
	{ "Poker",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Polo",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Pool",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Pro wrestling",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Public affairs",				EPG_EVENT_CONTENTMASK_SOCIALPOLITICALECONOMICS },
	{ "Racquet",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Religious",					EPG_EVENT_CONTENTMASK_ARTSCULTURE },
	{ "Ringuette",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Rodeo",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Roller derby",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Rowing",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Rugby",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Running",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Sailing",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Science",					EPG_EVENT_CONTENTMASK_EDUCATIONALSCIENCE },
	{ "Shooting",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Shopping",					EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Skateboarding",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Skating",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Skeleton",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Skiing",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Snooker",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Snowboarding",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Snowmobile",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Soccer",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Softball",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Speed skating",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Sports event",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Sports non-event",			EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Sports talk",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Squash",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Sumo wrestling",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Surfing",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Swimming",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Table tennis",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Tennis",						EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Theater",					EPG_EVENT_CONTENTMASK_ARTSCULTURE },
	{ "Track/field",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Triathlon",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Travel",						EPG_EVENT_CONTENTMASK_LEISUREHOBBIES },
	{ "Volleyball",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Watersports",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Water polo",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Water skiing",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Weather",					EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS },
	{ "Weightlifting",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Wrestling",					EPG_EVENT_CONTENTMASK_SPORTS },
	{ "Yacht racing",				EPG_EVENT_CONTENTMASK_SPORTS },
	{ nullptr,						EPG_EVENT_CONTENTMASK_UNDEFINED },
};

//---------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __GENREMAP_H_
