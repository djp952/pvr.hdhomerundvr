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

#include <algorithm>
#include <cctype>
#include <functional>
#include <kodi/addon-instance/PVR.h>
#include <libxml/xmlreader.h>
#include <list>
#include <map>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sqlite3ext.h>
#include <string>
#include <vector>
#include <version.h>

#include "curlshare.h"
#include "dbtypes.h"
#include "http_exception.h"
#include "string_exception.h"
#include "xmlstream.h"

extern "C" { SQLITE_EXTENSION_INIT1 };

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

// sqlext/uuid.c
//
extern "C" int sqlite3_uuid_init(sqlite3* db, char** pzErrMsg, const sqlite3_api_routines * pApi);

// sqlext/zipfile.c
//
extern "C" int sqlite3_zipfile_init(sqlite3* db, char** pzErrMsg, const sqlite3_api_routines *pApi);

// xmltv virtual table functions
//
int xmltv_bestindex(sqlite3_vtab* vtab, sqlite3_index_info* info);
int xmltv_close(sqlite3_vtab_cursor* cursor);
int xmltv_column(sqlite3_vtab_cursor* cursor, sqlite3_context* context, int ordinal);
int xmltv_connect(sqlite3* instance, void* aux, int argc, const char* const* argv, sqlite3_vtab** vtab, char** err);
int xmltv_disconnect(sqlite3_vtab* vtab);
int xmltv_eof(sqlite3_vtab_cursor* cursor);
int xmltv_filter(sqlite3_vtab_cursor* cursor, int indexnum, char const* indexstr, int argc, sqlite3_value** argv);
int xmltv_next(sqlite3_vtab_cursor* cursor);
int xmltv_open(sqlite3_vtab* vtab, sqlite3_vtab_cursor** cursor);
int xmltv_rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* rowid);

//---------------------------------------------------------------------------
// TYPE DECLARATIONS
//---------------------------------------------------------------------------

// byte_string
//
// Alias for an std::basic_string<> of bytes
typedef std::basic_string<uint8_t> byte_string;

// curl_writefunction
//
// Function pointer for a CURL write function implementation
typedef size_t(*curl_writefunction)(void const*, size_t, size_t, void*);

// json_get_aggregate_state
//
// Used as the state object for the json_get_aggregate function
typedef std::vector<std::tuple<std::string, std::string>> json_get_aggregate_state;

// xmltv_vtab_columns
//
// Constants indicating the xmltv virtual table column ordinals
enum class xmltv_vtab_columns {

	uri = 0,				// uri text hidden
	onchannel,				// onchannel pointer hidden
	channel,				// channel text
	start,					// start text
	stop,					// stop text
	title,					// title text
	subtitle,				// subtitle text
	desc,					// desc text
	date,					// date text
	categories,				// categories text
	language,				// language text
	iconsrc,				// iconsrc text
	seriesid,				// seriesid text
	episodenum,				// episodenum text
	programtype,			// programtype text
	isnew,					// isnew integer
	starrating,				// starrating text
};

// xmltv_vtab
//
// Subclassed version of sqlite3_vtab for the xmltv virtual table
struct xmltv_vtab : public sqlite3_vtab
{
	// Instance Constructor
	//
	xmltv_vtab() { memset(static_cast<sqlite3_vtab*>(this), 0, sizeof(sqlite3_vtab)); }
};

// xmltv_vtab_cursor
//
// Subclassed version of sqlite3_vtab_cursor for the xmltv virtual table
struct xmltv_vtab_cursor : public sqlite3_vtab_cursor
{
	// Instance Constructor
	//
	xmltv_vtab_cursor() { memset(static_cast<sqlite3_vtab_cursor*>(this), 0, sizeof(sqlite3_vtab_cursor)); }

	// Type Declarations
	//
	using channelmap_t = std::map<std::string, struct xmltv_channel>;

	// Fields
	//
	std::string					uri;					// XMLTV input stream URL
	xmltv_onchannel_callback	onchannel = nullptr;	// Channel information callback
	sqlite3_int64				rowid = 0;				// Current SQLite rowid
	bool						eof = false;			// EOF flag
	channelmap_t				channelmap;				// Channel mapping collection
	std::unique_ptr<xmlstream>	stream;					// xmlstream instance
	xmlParserInputBufferPtr		buffer = nullptr;		// xmlParserInputBuffer instance
	xmlTextReaderPtr			reader = nullptr;		// xmlTextReader instance
};

//---------------------------------------------------------------------------
// LIBXML2 HELPER FUNCTIONS
//---------------------------------------------------------------------------

// xmlNodeGetChildElement (local)
//
// Helper method to access a child element of the specified XML node
static xmlNodePtr xmlNodeGetChildElement(xmlNodePtr node, xmlChar const* element)
{
	if((node == nullptr) || (element == nullptr)) return nullptr;

	node = node->children;
	while((node != nullptr) && (xmlStrcmp(node->name, element) != 0)) node = node->next;
	return node;
};

// xmlTextReaderForEachChildElement (local)
//
// Helper method to iterate over each child element of the current XML reader element
static void xmlTextReaderForEachChildElement(xmlTextReaderPtr reader, xmlChar const* element, std::function<void(xmlNodePtr)> const& callback)
{
	if((reader == nullptr) || (element == nullptr)) return;

	xmlNodePtr node = xmlTextReaderExpand(reader);
	if(node != nullptr) node = node->children;

	while(node != nullptr) {

		if(xmlStrcmp(node->name, element) == 0) callback(node);
		node = node->next;
	}
}

// xmlTextReaderGetChildElement (local)
//
// Helper method to access a child element of the current XML reader element
static xmlNodePtr xmlTextReaderGetChildElement(xmlTextReaderPtr reader, xmlChar const* element)
{
	if((reader == nullptr) || (element == nullptr)) return nullptr;

	xmlNodePtr node = xmlTextReaderExpand(reader);
	if(node != nullptr) node = node->children;

	while((node != nullptr) && (xmlStrcmp(node->name, element) != 0)) node = node->next;
	return node;
};

// xmlTextReaderGetChildElementWithAttribute (local)
//
// Helper method to access a child element of the current XML reader element
static xmlNodePtr xmlTextReaderGetChildElementWithAttribute(xmlTextReaderPtr reader, xmlChar const* element, xmlChar const* attribute, xmlChar const* value)
{
	if((reader == nullptr) || (element == nullptr) || (attribute == nullptr)) return nullptr;

	xmlNodePtr node = xmlTextReaderExpand(reader);
	if(node != nullptr) node = node->children;

	while(node != nullptr) {

		if(xmlStrcmp(node->name, element) == 0) {

			xmlChar* prop = xmlGetProp(node, attribute);
			if(prop != nullptr) {

				int comparison = xmlStrcmp(prop, value);
				xmlFree(prop);
				if(comparison == 0) return node;
			}
		}

		node = node->next;
	}

	return node;
};

//---------------------------------------------------------------------------
// GLOBAL VARIABLES
//---------------------------------------------------------------------------

// g_curlshare
//
// Global curlshare instance to share resources among all cURL connections
static curlshare g_curlshare;

// g_useragent
//
// Static string to use as the User-Agent for database driven HTTP requests
static std::string g_useragent = "Kodi-PVR/" + std::string(ADDON_INSTANCE_VERSION_PVR) + " " + VERSION_PRODUCTNAME_ANSI + "/" + VERSION_VERSION3_ANSI;

// g_xmltv_module
//
// Defines the entry points for the xmltv virtual table
static sqlite3_module g_xmltv_module = {

	0,							// iVersion
	nullptr,					// xCreate
	xmltv_connect,				// xConnect
	xmltv_bestindex,			// xBestIndex
	xmltv_disconnect,			// xDisconnect
	nullptr,					// xDestroy
	xmltv_open,					// xOpen
	xmltv_close,				// xClose
	xmltv_filter,				// xFilter
	xmltv_next,					// xNext
	xmltv_eof,					// xEof
	xmltv_column,				// xColumn
	xmltv_rowid,				// xRowid
	nullptr,					// xUpdate
	nullptr,					// xBegin
	nullptr,					// xSync
	nullptr,					// xCommit
	nullptr,					// xRollback
	nullptr,					// xFindMethod
	nullptr,					// xRename
	nullptr,					// xSavepoint
	nullptr,					// xRelease
	nullptr,					// xRollbackTo
	nullptr						// xShadowName
};

//---------------------------------------------------------------------------
// clean_filename
//
// SQLite scalar function to clean invalid chars from a file name
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void clean_filename(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// Null or zero-length input string results in a zero-length output string
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_text(context, "", -1, SQLITE_STATIC);

	// isinvalid_char
	//
	// Returns -1 if the specified character is invalid for a filename on Windows or Unix
	auto isinvalid_char = [](char const& ch) -> bool { 

		// Exclude characters with a value between 0 and 31 (inclusive) as well as various
		// specific additional characters: [",<,>,|,:,*,?,\,/]
		return (((static_cast<int>(ch) >= 0) && (static_cast<int>(ch) <= 31)) ||
			(ch == '"') || (ch == '<') || (ch == '>') || (ch == '|') || (ch == ':') ||
			(ch == '*') || (ch == '?') || (ch == '\\') || (ch == '/'));
	};

	std::string output(str);
	output.erase(std::remove_if(output.begin(), output.end(), isinvalid_char), output.end());

	// Return the generated string as a transient value (needs to be copied)
	return sqlite3_result_text(context, output.c_str(), -1, SQLITE_TRANSIENT);
}

//---------------------------------------------------------------------------
// decode_channel_id
//
// SQLite scalar function to reverse encode_channel_id
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void decode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	union channelid			channelid;			// Encoded channel identifier

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid arguments", -1);
	
	// Null input results in "0"
	if(sqlite3_value_type(argv[0]) == SQLITE_NULL) return sqlite3_result_text(context, "0", -1, SQLITE_STATIC);

	// Convert the input encoded channelid back into a string
	channelid.value = static_cast<unsigned int>(sqlite3_value_int(argv[0]));
	char* result = (channelid.parts.subchannel > 0) ? sqlite3_mprintf("%u.%u", channelid.parts.channel, channelid.parts.subchannel) :
		sqlite3_mprintf("%u", channelid.parts.channel);

	// Return the converted string as the scalar result from this function
	return sqlite3_result_text(context, result, -1, sqlite3_free);
}

//---------------------------------------------------------------------------
// decode_star_rating
//
// SQLite scalar function to decode a star rating string
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void decode_star_rating(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	float			divisor = 10.0F;			// Parsed rating divisor
	float			dividend = 0.0F;			// Parsed rating dividend

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// Null or zero-length input string results in 0
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_int(context, 0);

	// Best guess is that this always comes in as x.x/x.x or just x.x ...
	int parts = sscanf(str, "%f/%f", &dividend, &divisor);
	if((parts >= 1) && (divisor > 0.0F) && (dividend >= 0.0F)) return sqlite3_result_int(context, static_cast<int>((dividend / divisor) * 10.0F));

	return sqlite3_result_int(context, 0);
}

//---------------------------------------------------------------------------
// encode_channel_id
//
// SQLite scalar function to generate a channel identifier
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void encode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int			channel = 0;			// Parsed channel number
	int			subchannel = 0;			// Parsed subchannel number

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid arguments", -1);
	
	// Null or zero-length input string results in 0
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_int(context, 0);

	// The input format must be %d.%d or %d
	if((sscanf(str, "%d.%d", &channel, &subchannel) == 2) || (sscanf(str, "%d", &channel) == 1)) {

		// Construct the channel identifier by setting the bit field components
		union channelid channelid;
		channelid.parts.channel = channel;
		channelid.parts.subchannel = subchannel;

		return sqlite3_result_int(context, channelid.value);
	}

	// Could not parse the channel number into channel/subchannel components
	return sqlite3_result_int(context, 0);
}

//---------------------------------------------------------------------------
// fnv_hash
//
// SQLite scalar function to generate an FNV-1a hash code from multiple values
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void fnv_hash(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	// 32-bit FNV-1a primes (http://www.isthe.com/chongo/tech/comp/fnv/index.html#FNV-source) 
	const int fnv_offset_basis = 2166136261U;
	const int fnv_prime = 16777619U;

	if(argc == 0) return sqlite3_result_int(context, 0);

	// Calcuate the FNV-1a hash for each argument passed into the function
	int hash = fnv_offset_basis;
	for(int index = 0; index < argc; index++) {

		int type = sqlite3_value_type(argv[index]);

		// SQLITE_NULL - Ignore this value
		if(type == SQLITE_NULL) continue;

		// Treat SQLITE_INTEGER values as integers
		else if(type == SQLITE_INTEGER) {
			
			hash ^= sqlite3_value_int(argv[index]);
			hash *= fnv_prime;
		}

		// Treat everything else as a blob, per documentation SQLite will cast
		// SQLITE_FLOAT and SQLITE_TEXT into blobs directly without conversion
		else {

			uint8_t const* blob = reinterpret_cast<uint8_t const*>(sqlite3_value_blob(argv[index]));
			if(blob == nullptr) continue;

			// Hash each byte of the BLOB individually
			for(int offset = 0; offset < sqlite3_value_bytes(argv[index]); offset++) {
				
				hash ^= blob[offset];
				hash *= fnv_prime;
			}
		}
	}

	return sqlite3_result_int(context, hash);
}

//---------------------------------------------------------------------------
// get_channel_number
//
// SQLite scalar function to read the channel number from a string
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void get_channel_number(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int				channel = 0;				// Parsed channel number
	int				subchannel = 0;				// Parsed subchannel number

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);
	
	// Null or zero-length input string results in 0
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_int(context, 0);

	// The input format must be %d.%d or %d
	if((sscanf(str, "%d.%d", &channel, &subchannel) == 2) || (sscanf(str, "%d", &channel) == 1)) return sqlite3_result_int(context, channel);
	else return sqlite3_result_int(context, 0);
}

//---------------------------------------------------------------------------
// get_episode_number
//
// SQLite scalar function to read the episode number from a string
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void get_episode_number(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int				season = -1;				// Parsed season number
	int				episode = -1;				// Parsed episode number

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);
	
	// Null or zero-length input string results in -1
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_int(context, -1);

	if(sscanf(str, "S%dE%d", &season, &episode) == 2) return sqlite3_result_int(context, episode);
	else if(sscanf(str, "%d-%d", &season, &episode) == 2) return sqlite3_result_int(context, episode);
	else if(sscanf(str, "EP%d", &episode) == 1) return sqlite3_result_int(context, episode);
	else if(sscanf(str, "%d", &episode) == 1) return sqlite3_result_int(context, episode);

	return sqlite3_result_int(context, -1);
}

//---------------------------------------------------------------------------
// get_primary_genre
//
// SQLite scalar function to get the primary genre string from a list
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void get_primary_genre(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// A null or zero-length string results in a NULL result
	const char* input = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((input == nullptr) || (*input == 0)) return sqlite3_result_null(context);

	// The genre strings are comma-delimited, chop off everything else
	char const* comma = strchr(input, ',');
	sqlite3_result_text(context, input, (comma != nullptr) ? static_cast<int>(comma - input) : -1, SQLITE_TRANSIENT);
}

//---------------------------------------------------------------------------
// get_recording_id
//
// SQLite scalar function to read the recording ID from a command URL
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void get_recording_id(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// This function accepts the command URL for the recording so that the identifier can be extracted
	const char* url = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((url == nullptr) || (*url == 0)) return sqlite3_result_error(context, "url argument is null or zero-length", -1);

	// Allocate a CURLU instance to parse the command URL components
	CURLU* curlu = curl_url();
	if(curlu == nullptr) return sqlite3_result_error(context, "insufficient memory to allocate CURLU instance", -1);

	// Apply the command URL to the CURLU instance
	CURLUcode curluresult = curl_url_set(curlu, CURLUPart::CURLUPART_URL, url, 0);
	if(curluresult == CURLUE_OK) {

		// We are interested in the query string portion of the CmdURL
		char* querystring = nullptr;
		curluresult = curl_url_get(curlu, CURLUPART_QUERY, &querystring, 0);
		if(curluresult == CURLUE_OK) {

			// The query string must start with "id=", use the rest as-is.  This will be OK for now, but
			// a more robust solution would be parsing the entire query string and selecting just the id key/value
			if(strncasecmp(querystring, "id=", 3) == 0) sqlite3_result_text(context, &querystring[3], -1, SQLITE_TRANSIENT);
			else sqlite3_result_error(context, "unable to extract recording id from specified url", -1);

			curl_free(querystring);			// Release the allocated query string
		}

		else sqlite3_result_error(context, "unable to extract query string from specified url", -1);
	}

	else sqlite3_result_error(context, "unable to parse supplied url", -1);

	curl_url_cleanup(curlu);				// Release the CURLU instance
}

//---------------------------------------------------------------------------
// get_season_number
//
// SQLite scalar function to read the season number from a string
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void get_season_number(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int				season = -1;				// Parsed season number
	int				episode = -1;				// Parsed episode number

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);
	
	// Null or zero-length input string results in -1
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_int(context, -1);

	if(sscanf(str, "S%dE%d", &season, &episode) == 2) return sqlite3_result_int(context, season);
	else if(sscanf(str, "%d-%d", &season, &episode) == 2) return sqlite3_result_int(context, season);

	return sqlite3_result_int(context, -1);
}

//---------------------------------------------------------------------------
// json_get
//
// SQLite scalar function to generate a JSON document
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

static void json_get(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	bool					post = false;			// Flag indicating an HTTP POST operation
	std::string				postfields;				// HTTP post fields (optional)
	long					responsecode = 200;		// HTTP response code
	std::vector<uint8_t>	blob;					// HTTP response data buffer
	rapidjson::Document		document;				// Resultant JSON document

	// json_get requires at least the URL argument to be specified, with an optional second
	// argument indicating the method (GET/POST), and an optional third argument to specify
	// the post fields if the POST method was specified
	if((argc < 1) || (argc > 3) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);
												
	// A null or zero-length URL results in null
	const char* url = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((url == nullptr) || (*url == '\0')) return sqlite3_result_null(context);

	// Check for HTTP POST operation
	if((argc >= 2) && (argv[1] != nullptr)) {

		const char* method = reinterpret_cast<const char*>(sqlite3_value_text(argv[1]));
		if(method != nullptr) post = (strncasecmp(method, "POST", 4) == 0);
	}

	// Check for HTTP POST field data
	if((argc >= 3) && (argv[2] != nullptr)) {

		const char* postdata = reinterpret_cast<const char*>(sqlite3_value_text(argv[2]));
		if(postdata != nullptr) postfields.assign(postdata);
	}

	// Create a write callback for libcurl to invoke to write the data
	auto write_function = [](void const* data, size_t size, size_t count, void* userdata) -> size_t {
	
		// Acquire the pointer to the std::vector<> and retrieve the current size
		std::vector<uint8_t>* blob = reinterpret_cast<std::vector<uint8_t>*>(userdata);
		size_t blobsize = blob->size();

		// Resize the vector<> as needed and copy in the data from cURL
		blob->resize(blobsize + (size * count));
		memcpy(&blob->data()[blobsize], data, size * count);

		return (size * count);
	};

	// Set the rapdison document type
	document.SetObject();

#if defined(_WINDOWS) && defined(_DEBUG)
	// Dump the target URL to the debugger on Windows _DEBUG builds to watch for URL duplication
	char debugurl[512];
	snprintf(debugurl, std::extent<decltype(debugurl)>::value, "%s (%s): %s%s%s%s\r\n", __func__, (post) ? "post" : "get", url, (post) ? " [" : "", (post) ? postfields.c_str() : "", (post) ? "]" : "");
	OutputDebugStringA(debugurl);
#endif

	// Initialize the CURL session for the download operation
	CURL* curl = curl_easy_init();
	if(curl == nullptr) return sqlite3_result_error(context, "cannot initialize libcurl object", -1);

	// Set the CURL options and execute the web request, switching to POST if indicated
	CURLcode curlresult = curl_easy_setopt(curl, CURLOPT_URL, url);
	if((post) && (curlresult == CURLE_OK)) curlresult = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields.c_str());
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_USERAGENT, g_useragent.c_str());
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity, gzip, deflate");
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<curl_writefunction>(write_function));
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&blob));
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(g_curlshare));
	if(curlresult == CURLE_OK) curlresult = curl_easy_perform(curl);
	if(curlresult == CURLE_OK) curlresult = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responsecode);
	curl_easy_cleanup(curl);

	// Check if any of the above operations failed and return an error condition
	if(curlresult != CURLE_OK) {

		// Use sqlite3_mprintf to generate the formatted error message
		auto message = sqlite3_mprintf("http %s request on [%s] failed: %s", (post) ? "post" : "get", url, curl_easy_strerror(curlresult));
		sqlite3_result_error(context, message, -1);
		return sqlite3_free(reinterpret_cast<void*>(message));
	}

	// Check the HTTP response code and return an error condition if unsuccessful
	if((responsecode < 200) || (responsecode > 299)) {

		// Use sqlite3_mprintf to generate the formatted error message
		auto message = sqlite3_mprintf("http %s request on url [%s] failed with http response code %ld", (post) ? "post" : "get", url, responsecode);
		sqlite3_result_error(context, message, -1);
		return sqlite3_free(reinterpret_cast<void*>(message));
	}

	// Ensure the BLOB data is null-terminated before attempting to parse it as JSON
	blob.push_back(static_cast<uint8_t>('\0'));

	// Parse the JSON data returned from the cURL operation
	document.ParseInsitu(reinterpret_cast<char*>(blob.data()));
	if(document.HasParseError()) return (document.GetParseError() == rapidjson::ParseErrorCode::kParseErrorDocumentEmpty) ? 
		sqlite3_result_null(context) : sqlite3_result_error(context, rapidjson::GetParseError_En(document.GetParseError()), -1);

	// If the document contains no data, return null
	if(document.IsNull() || (document.IsObject() && (document.MemberCount() == 0)) || (document.IsArray() && (document.Size() == 0))) return sqlite3_result_null(context);

	// Serialize the document back into a JSON string
	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
	document.Accept(writer);

	// Return the resultant JSON back to the caller as a text string
	return sqlite3_result_text(context, sb.GetString(), -1, SQLITE_TRANSIENT);
}

//---------------------------------------------------------------------------
// json_get_aggregate_final
//
// SQLite aggregate function to generate a JSON array of multiple JSON documents
//
// Arguments:
//
//	context		- SQLite context object

void json_get_aggregate_final(sqlite3_context* context)
{
	rapidjson::Document			document;			// Resultant JSON document

	// Retrieve the json_get_aggregate_state pointer from the aggregate context; if it does not exist return NULL
	json_get_aggregate_state** statepp = reinterpret_cast<json_get_aggregate_state**>(sqlite3_aggregate_context(context, sizeof(json_get_aggregate_state*)));
	json_get_aggregate_state* statep = (statepp == nullptr) ? nullptr : *statepp;
	if(statep == nullptr) { sqlite3_aggregate_context(context, 0); return sqlite3_result_null(context); }

	// Create a write callback for libcurl to invoke to write the data
	auto write_function = [](void const* data, size_t size, size_t count, void* userdata) -> size_t {

		// Acquire the pointer to the std::vector<> and retrieve the current size
		std::vector<uint8_t>* blob = reinterpret_cast<std::vector<uint8_t>*>(userdata);
		size_t blobsize = blob->size();

		// Resize the vector<> as needed and copy in the data from cURL
		blob->resize(blobsize + (size * count));
		memcpy(&blob->data()[blobsize], data, size * count);

		return (size * count);
	};

	// Wrap the json_get_aggregate_state pointer in a unique_ptr to automatically release it when it falls out of scope
	std::unique_ptr<json_get_aggregate_state, std::function<void(json_get_aggregate_state*)>> state(statep, [&](json_get_aggregate_state* statep) -> void {

		if(statep) delete statep;					// Release the json_get_aggregate_state
		sqlite3_aggregate_context(context, 0);		// Clear the aggregate context
	});

	// Initialize a cURL multiple interface session to handle the data transfers
	std::unique_ptr<CURLM, std::function<void(CURLM*)>> curlm(curl_multi_init(), [](CURLM* curlm) -> void { curl_multi_cleanup(curlm); });
	if(!curlm) throw string_exception(__func__, ": curl_multi_init() failed");

	// Set the rapdison document type
	document.SetObject();

	try {

		// Disable pipelining/multiplexing on the multi interface object. It doesn't make an appreciable
		// performance difference here and may have been the root cause of a lot of weird problems
		CURLMcode curlmresult = curl_multi_setopt(curlm.get(), CURLMOPT_PIPELINING, CURLPIPE_NOTHING);
		if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_setopt(CURLMOPT_PIPELINING) failed: ", curl_multi_strerror(curlmresult));

		// Create a list<> to hold the individual transfer information (no iterator invalidation)
		std::list<std::tuple<CURL*, std::vector<uint8_t>, std::string>> transfers;

		try {

			for(auto const& iterator : *state) {

				// Create and initialize the cURL easy interface handle for this transfer operation
				CURL* curl = curl_easy_init();
				if(curl == nullptr) throw string_exception(__func__, ": curl_easy_init() failed");

			#if defined(_WINDOWS) && defined(_DEBUG)
				// Dump the target URL to the debugger on Windows _DEBUG builds to watch for URL duplication
				char debugurl[256];
				snprintf(debugurl, std::extent<decltype(debugurl)>::value, "%s: %s\r\n", __func__, std::get<0>(iterator).c_str());
				OutputDebugStringA(debugurl);
			#endif

				// Create the transfer instance to track this operation in the list<>
				auto transfer = transfers.emplace(transfers.end(), std::make_tuple(curl, std::vector<uint8_t>(), std::get<1>(iterator)));

				// Set the CURL options and execute the web request to get the JSON string data
				CURLcode curlresult = curl_easy_setopt(curl, CURLOPT_URL, std::get<0>(iterator).c_str());
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_USERAGENT, g_useragent.c_str());
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity, gzip, deflate");
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<curl_writefunction>(write_function));
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&std::get<1>(*transfer)));
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(g_curlshare));

				// Verify that initialization of the cURL easy interface handle was completed successfully
				if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed: ", curl_easy_strerror(curlresult));
			}

			// Add all of the generated cURL easy interface objects to the cURL multi interface object
			for(auto const& transfer : transfers) {

				curlmresult = curl_multi_add_handle(curlm.get(), std::get<0>(transfer));
				if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_add_handle() failed: ", curl_multi_strerror(curlmresult));
			}

			// Execute the transfer operation(s) until they have all completed
			int numfds = 0;
			curlmresult = curl_multi_perform(curlm.get(), &numfds);
			while((curlmresult == CURLM_OK) && (numfds > 0)) {

				curlmresult = curl_multi_wait(curlm.get(), nullptr, 0, 500, &numfds);
				if(curlmresult == CURLM_OK) curlmresult = curl_multi_perform(curlm.get(), &numfds);
			}

			// After the transfer operation(s) have completed, verify the HTTP status of each one
			// and abort the operation if any of them did not return HTTP 200: OK
			for(auto& transfer : transfers) {

				long responsecode = 200;			// Assume HTTP 200: OK

				// The response code will come back as zero if there was no response from the host,
				// otherwise it should be a standard HTTP response code
				curl_easy_getinfo(std::get<0>(transfer), CURLINFO_RESPONSE_CODE, &responsecode);

				if(responsecode == 0) throw string_exception(__func__, ": no response from host");
				else if((responsecode < 200) || (responsecode > 299)) throw http_exception(responsecode);

				// Ensure the BLOB data is null-terminated before attempting to parse it as JSON
				std::get<1>(transfer).push_back(static_cast<uint8_t>('\0'));

				// Parse the JSON data returned from the cURL operation
				rapidjson::Document json;
				json.ParseInsitu(reinterpret_cast<char*>(std::get<1>(transfer).data()));

				// If the document failed to parse skip it if it's an empty document; otherwise throw an exception
				if(json.HasParseError()) {

					if(json.GetParseError() == rapidjson::ParseErrorCode::kParseErrorDocumentEmpty) continue;
					else throw string_exception(rapidjson::GetParseError_En(json.GetParseError()));
				}

				// Check if the document is null or contained no members/elements
				if(json.IsNull() || (json.IsObject() && (json.MemberCount() == 0)) || (json.IsArray() && (json.Size() == 0))) continue;

				// Add the entire document as a new array element in the final document
				document.AddMember(rapidjson::GenericStringRef<char>(std::get<2>(transfer).c_str()), rapidjson::Value(json, document.GetAllocator()), document.GetAllocator());
			}

			// Clean up and destroy the generated cURL easy interface handles
			for(auto& transfer : transfers) {

				curl_multi_remove_handle(curlm.get(), std::get<0>(transfer));
				curl_easy_cleanup(std::get<0>(transfer));
			}
		}

		// Clean up any destroy any created cURL easy interface handles on exception
		catch(...) {

			for(auto& transfer : transfers) {

				curl_multi_remove_handle(curlm.get(), std::get<0>(transfer));
				curl_easy_cleanup(std::get<0>(transfer));
			}

			throw;
		}

		// If the generated document has nothing in it return null as the query result
		if(document.MemberCount() == 0) return sqlite3_result_null(context);

		// Serialize the document back into a JSON string
		rapidjson::StringBuffer sb;
		rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
		document.Accept(writer);

		// Return the resultant JSON back to the caller as a text string
		return sqlite3_result_text(context, sb.GetString(), -1, SQLITE_TRANSIENT);
	}

	catch(std::exception const& ex) { return sqlite3_result_error(context, ex.what(), -1); }
	catch(...) { return sqlite3_result_error_code(context, SQLITE_ERROR); }
}

//---------------------------------------------------------------------------
// json_get_aggregate_step
//
// SQLite aggregate function to generate a JSON array of multiple JSON documents
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void json_get_aggregate_step(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if(argc != 2) return sqlite3_result_error(context, "invalid argument", -1);
	if(argv[0] == nullptr) return sqlite3_result_error(context, "invalid argument", -1);

	// Use a json_get_aggregate_state to hold the aggregate state as it's calculated
	json_get_aggregate_state** statepp = reinterpret_cast<json_get_aggregate_state**>(sqlite3_aggregate_context(context, sizeof(json_get_aggregate_state*)));
	if(statepp == nullptr) return sqlite3_result_error_nomem(context);
	
	// If the string_vector hasn't been allocated yet, allocate it now
	json_get_aggregate_state* statep = *statepp;
	if(statep == nullptr) *statepp = statep = new json_get_aggregate_state();
	if(statep == nullptr) return sqlite3_result_error_nomem(context);

	// There are two arguments to this function, the first is the URL to query for the JSON
	// and the second is the key name to assign to the resultant JSON array element
	char const* url = reinterpret_cast<char const*>(sqlite3_value_text(argv[0]));
	char const* key = reinterpret_cast<char const*>(sqlite3_value_text(argv[1]));

	// The URL string must be non-null, but the key can be null or blank if the caller doesn't care
	if(url != nullptr) statep->emplace_back(std::make_tuple(url, (key != nullptr) ? key : ""));
	else return sqlite3_result_error(context, "invalid argument", -1);
}

//---------------------------------------------------------------------------
// url_encode
//
// SQLite scalar function to encode a string with URL escape sequences
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void url_encode(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// A null or zero-length string results in a NULL result
	const char* input = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((input == nullptr) || (*input == 0)) return sqlite3_result_null(context);

	// Initialize the CURL session for the string operation
	CURL* curl = curl_easy_init();
	if(curl == nullptr) return sqlite3_result_error(context, "cannot initialize libcurl object", -1);

	// Use libcurl to encode any required escape sequences
	sqlite3_result_text(context, curl_easy_escape(curl, input, 0), -1, curl_free);

	curl_easy_cleanup(curl);
}

//---------------------------------------------------------------------------
// url_remove_query_string
//
// SQLite scalar function to remove the query string portion of a URL
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void url_remove_query_string(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// A null or zero-length string results in a NULL result
	const char* input = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((input == nullptr) || (*input == 0)) return sqlite3_result_null(context);

	// Chop off everything in the string after the first occurrence of a question mark
	char const* questionmark = strchr(input, '?');
	sqlite3_result_text(context, input, (questionmark != nullptr) ? static_cast<int>(questionmark - input) : -1, SQLITE_TRANSIENT);
}

//---------------------------------------------------------------------------
// xmltv_bestindex
//
// Determines the best index to use when querying the virtual table
//
// Arguments:
//
//	vtab	- Virtual Table instance
//	info	- Selected index information to populate

int xmltv_bestindex(sqlite3_vtab* /*vtab*/, sqlite3_index_info* info)
{
	// usable_constraint_index (local)
	//
	// Finds the first usable constraint for the specified column ordinal
	auto usable_constraint_index = [](sqlite3_index_info* info, int ordinal) -> int {

		// The constraints aren't necessarily in the order specified by the table, loop to find it
		for(int index = 0; index < info->nConstraint; index++) {

			auto constraint = &info->aConstraint[index];
			if(constraint->iColumn == ordinal) return ((constraint->usable) && (constraint->op == SQLITE_INDEX_CONSTRAINT_EQ)) ? index : -1;
		}

		return -1;
	};

	// argv[1] - uri; required
	int uri  = usable_constraint_index(info, static_cast<int>(xmltv_vtab_columns::uri));
	if(uri < 0) return SQLITE_CONSTRAINT;
	info->aConstraintUsage[uri].argvIndex = 1;
	info->aConstraintUsage[uri].omit = 1;

	// argv[2] - onchannel; optional
	int onchannel = usable_constraint_index(info, static_cast<int>(xmltv_vtab_columns::onchannel));
	if(onchannel >= 0) {

		info->aConstraintUsage[onchannel].argvIndex = 2;
		info->aConstraintUsage[onchannel].omit = 1;
	}

	// There are no viable indexes on this virtual table, force the cost to 1
	info->estimatedCost = 1.0;

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// xmltv_close
//
// Closes and deallocates a virtual table cursor instance
//
// Arguments:
//
//	cursor		- Cursor instance allocated by xOpen

int xmltv_close(sqlite3_vtab_cursor* cursor)
{
	// Cast the provided generic cursor instance back into an xmltv_vtab_cursor instance
	xmltv_vtab_cursor* xmltvcursor = reinterpret_cast<xmltv_vtab_cursor*>(cursor);
	assert(xmltvcursor != nullptr);

	// Free the embedded text reader and input buffer instances
	if(xmltvcursor->reader != nullptr) xmlFreeTextReader(xmltvcursor->reader);
	if(xmltvcursor->buffer != nullptr) xmlFreeParserInputBuffer(xmltvcursor->buffer);

	// Ensure the underlying xmlstream instance has been closed
	if(xmltvcursor->stream) xmltvcursor->stream->close();

	delete xmltvcursor;

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// xmltv_column
//
// Accesses the data in the specified column of the current cursor row
//
// Arguments:
//
//	cursor		- Virtual table cursor instance
//	context		- Result context object
//	ordinal		- Ordinal of the column being accessed

int xmltv_column(sqlite3_vtab_cursor* cursor, sqlite3_context* context, int ordinal)
{
	xmlNodePtr			node = nullptr;			// Pointer for accessing child elements

	// Cast the provided generic cursor instance back into an xmltv_vtab_cursor instance
	xmltv_vtab_cursor* xmltvcursor = reinterpret_cast<xmltv_vtab_cursor*>(cursor);
	assert(xmltvcursor != nullptr);

	// Assume that the result will be NULL to avoid adding this for each column test below
	sqlite3_result_null(context);

	// Extract the requested value from the current <programme> element in the XML document
	switch(static_cast<xmltv_vtab_columns>(ordinal)) {

		case xmltv_vtab_columns::uri:
			sqlite3_result_text(context, xmltvcursor->uri.c_str(), -1, SQLITE_TRANSIENT);
			break;

		// Special case: never expose the pointer to the onchannel() callback function
		case xmltv_vtab_columns::onchannel:
			break;

		case xmltv_vtab_columns::channel:
			sqlite3_result_text(context, reinterpret_cast<char*>(xmlTextReaderGetAttribute(xmltvcursor->reader, BAD_CAST("channel"))), -1, xmlFree); 
			break;

		case xmltv_vtab_columns::start:
			sqlite3_result_text(context, reinterpret_cast<char*>(xmlTextReaderGetAttribute(xmltvcursor->reader, BAD_CAST("start"))), -1, xmlFree);
			break;

		case xmltv_vtab_columns::stop:
			sqlite3_result_text(context, reinterpret_cast<char*>(xmlTextReaderGetAttribute(xmltvcursor->reader, BAD_CAST("stop"))), -1, xmlFree);
			break;

		case xmltv_vtab_columns::title:
			node = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("title"));
			if(node != nullptr) sqlite3_result_text(context, reinterpret_cast<char*>(xmlNodeGetContent(node)), -1, xmlFree);
			break;

		case xmltv_vtab_columns::subtitle:
			node = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("sub-title"));
			if(node != nullptr) sqlite3_result_text(context, reinterpret_cast<char*>(xmlNodeGetContent(node)), -1, xmlFree);
			break;

		case xmltv_vtab_columns::desc:
			node = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("desc"));
			if(node != nullptr) sqlite3_result_text(context, reinterpret_cast<char*>(xmlNodeGetContent(node)), -1, xmlFree);
			break;

		case xmltv_vtab_columns::date:
			node = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("date"));
			if(node != nullptr) sqlite3_result_text(context, reinterpret_cast<char*>(xmlNodeGetContent(node)), -1, xmlFree);
			break;

		// Special case: concatenate all of the <category> element values into a comma-delimted string
		case xmltv_vtab_columns::categories: 
			{
				std::string collector;				// Collector for the disparate strings
				bool progtype = false;				// Flag to skip progType <category> element

				xmlTextReaderForEachChildElement(xmltvcursor->reader, BAD_CAST("category"), [&](xmlNodePtr node) -> void {

					// The first <category> element is the progType, which we don't want to use for anything
					if(!progtype) { progtype = true; return; }

					xmlChar* value = xmlNodeGetContent(node);
					if(value != nullptr) {

						if(!collector.empty()) collector.append(",");
						collector.append(reinterpret_cast<char*>(value));
						xmlFree(value);
					}
				});

				if(!collector.empty()) sqlite3_result_text(context, collector.c_str(), -1, SQLITE_TRANSIENT);
			}
			break;

		case xmltv_vtab_columns::language:
			node = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("language"));
			if(node != nullptr) sqlite3_result_text(context, reinterpret_cast<char*>(xmlNodeGetContent(node)), -1, xmlFree);
			break;

		case xmltv_vtab_columns::iconsrc:
			node = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("icon"));
			if(node != nullptr) sqlite3_result_text(context, reinterpret_cast<char*>(xmlGetProp(node, BAD_CAST("src"))), -1, xmlFree);
			break;

		// Special case: the series-id tag will typically be qualified with system=cseries, but some items like Movies (programtype MV) will
		// not be qualified with that attribute.  Try system=cseries first, then use any series-id node
		case xmltv_vtab_columns::seriesid:
			node = xmlTextReaderGetChildElementWithAttribute(xmltvcursor->reader, BAD_CAST("series-id"), BAD_CAST("system"), BAD_CAST("cseries"));
			if(node == nullptr) node = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("series-id"));
			if(node != nullptr) sqlite3_result_text(context, reinterpret_cast<char*>(xmlNodeGetContent(node)), -1, xmlFree);
			break;

		case xmltv_vtab_columns::episodenum:
			node = xmlTextReaderGetChildElementWithAttribute(xmltvcursor->reader, BAD_CAST("episode-num"), BAD_CAST("system"), BAD_CAST("onscreen"));
			if(node != nullptr) sqlite3_result_text(context, reinterpret_cast<char*>(xmlNodeGetContent(node)), -1, xmlFree);
			break;

		// Special case: extract the program type from the alphanumeric identifer at the start of the dd_progid
		case xmltv_vtab_columns::programtype:
		{
			node = xmlTextReaderGetChildElementWithAttribute(xmltvcursor->reader, BAD_CAST("episode-num"), BAD_CAST("system"), BAD_CAST("dd_progid"));
			if(node != nullptr) {

				xmlChar* progid = xmlNodeGetContent(node);
				if(progid != nullptr) {

					if(strlen(reinterpret_cast<char*>(progid)) >= 2) sqlite3_result_text(context, reinterpret_cast<char*>(progid), 2, SQLITE_TRANSIENT);
					xmlFree(progid);
				}
			}
		}
		break;

		case xmltv_vtab_columns::isnew:
			node = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("new"));
			if(node != nullptr) sqlite3_result_int(context, 1);
			break;

		case xmltv_vtab_columns::starrating:
			node = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("star-rating"));
			if(node != nullptr) node = xmlNodeGetChildElement(node, BAD_CAST("value"));
			if(node != nullptr) sqlite3_result_text(context, reinterpret_cast<char*>(xmlNodeGetContent(node)), -1, xmlFree);
			break;
	}

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// xmltv_create
//
// Creates the specified virtual table
//
// Arguments:
//
//	instance	- SQLite database instance handle
//	aux			- Client data pointer provided to sqlite3_create_module[_v2]()
//	argc		- Number of provided metadata strings
//	argv		- Metadata strings
//	vtab		- On success contains the allocated virtual table instance
//	err			- On error contains a string-based error message

int xmltv_connect(sqlite3* instance, void* /*aux*/, int /*argc*/, const char* const* /*argv*/, sqlite3_vtab** vtab, char** err)
{
	// Declare the schema for the virtual table, use hidden columns for all of the filter criteria
 	int result = sqlite3_declare_vtab(instance, "create table xmltv(uri text hidden, onchannel pointer hidden, channel text, start text, "
		"stop text, title text, subtitle text, desc text, date text, categories text, language text, iconsrc text, seriesid text, "
		"episodenum text, programtype text, isnew integer, starrating text)");
	if(result != SQLITE_OK) return result;

	// Allocate and initialize the custom virtual table class
	try { *vtab = static_cast<sqlite3_vtab*>(new xmltv_vtab()); } 
	catch(std::exception const& ex) { *err = sqlite3_mprintf("%s", ex.what()); return SQLITE_ERROR; } 
	catch(...) { return SQLITE_ERROR; }

	return (*vtab == nullptr) ? SQLITE_NOMEM : SQLITE_OK;
}

//---------------------------------------------------------------------------
// xmltv_disconnect
//
// Disconnects from the xmltv virtual table
//
// Arguments:
//
//	vtab		- Virtual table instance allocated by xConnect

int xmltv_disconnect(sqlite3_vtab* vtab)
{
	if(vtab != nullptr) delete reinterpret_cast<xmltv_vtab*>(vtab);

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// xmltv_eof
//
// Determines if the specified cursor has moved beyond the last row of data
//
// Arguments:
//
//	cursor		- Virtual table cursor instance

int xmltv_eof(sqlite3_vtab_cursor* cursor)
{
	// Cast the provided generic cursor instance back into an xmltv_vtab_cursor instance
	xmltv_vtab_cursor* xmltvcursor = reinterpret_cast<xmltv_vtab_cursor*>(cursor);
	assert(xmltvcursor != nullptr);

	return (xmltvcursor->eof) ? 1 : 0;
}

//---------------------------------------------------------------------------
// xmltv_filter
//
// Executes a search of the virtual table
//
// Arguments:
//
//	cursor		- Virtual table cursor instance
//	indexnum	- Virtual table index number from xBestIndex()
//	indexstr	- Virtual table index string from xBestIndex()
//	argc		- Number of arguments assigned by xBestIndex()
//	argv		- Argument data assigned by xBestIndex()

int xmltv_filter(sqlite3_vtab_cursor* cursor, int /*indexnum*/, char const* /*indexstr*/, int argc, sqlite3_value** argv)
{
	// Cast the provided generic cursor instance back into an xmltv_vtab_cursor instance
	xmltv_vtab_cursor* xmltvcursor = reinterpret_cast<xmltv_vtab_cursor*>(cursor);
	assert(xmltvcursor != nullptr);

	// input_read_callback (local)
	//
	// xmlParserInputBuffer callback function
	auto input_read_callback = [](void* context, char* buffer, int len) -> int
	{
		xmltv_vtab_cursor* xmltvcursor = reinterpret_cast<xmltv_vtab_cursor*>(context);
		assert(xmltvcursor != nullptr);

		try { return static_cast<int>(xmltvcursor->stream->read(reinterpret_cast<uint8_t*>(buffer), len)); } 
		catch(...) { return -1; }
	};

	// input_close_callback (local)
	//
	// xmlParserInputBuffer callback function
	auto input_close_callback = [](void* context) -> int
	{
		xmltv_vtab_cursor* xmltvcursor = reinterpret_cast<xmltv_vtab_cursor*>(context);
		assert(xmltvcursor != nullptr);

		xmltvcursor->stream->close();
		return 0;
	};

	try {

		// The uri argument must have been specified by xBestIndex
		if(argc < 1) throw string_exception(__func__, ": invalid argument count provided by xBestIndex");

		// Assign the uri string to the xmltv_vtab_cursor instance
		char const* uri = reinterpret_cast<char const*>(sqlite3_value_text(argv[0]));
		if(uri != nullptr) xmltvcursor->uri.assign(uri);
		if(xmltvcursor->uri.empty()) throw string_exception(__func__, ": null or zero-length uri string");

		// The onchannel argument is optional and may have been specified by xBestIndex if the calling
		// function was capable of providing this callback
		if(argc >= 2) {

			void* onchannelptr = sqlite3_value_pointer(argv[1], typeid(xmltv_onchannel_callback).name());
			if(onchannelptr) xmltvcursor->onchannel = *reinterpret_cast<xmltv_onchannel_callback*>(onchannelptr);
		}

	#if defined(_WINDOWS) && defined(_DEBUG)
		// Dump the target URI to the debugger on Windows _DEBUG builds
		char debugurl[256];
		snprintf(debugurl, std::extent<decltype(debugurl)>::value, "%s: %s\r\n", __func__, uri);
		OutputDebugStringA(debugurl);
	#endif

		// Create the xmlstream instance that will take care of streaming the XMLTV data
		xmltvcursor->stream = xmlstream::create(uri, g_useragent.c_str(), g_curlshare);

		// Set up the xmlParserInputBuffer and the xmlTextReader instances around the XMLTV stream
		xmltvcursor->buffer = xmlParserInputBufferCreateIO(input_read_callback, input_close_callback, xmltvcursor, xmlCharEncoding::XML_CHAR_ENCODING_UTF8);
		xmltvcursor->reader = xmlNewTextReader(xmltvcursor->buffer, nullptr);
	}
	
	catch(std::exception const& ex) { xmltvcursor->pVtab->zErrMsg = sqlite3_mprintf("%s", ex.what()); return SQLITE_ERROR; } 
	catch(...) { return SQLITE_ERROR; }

	// xFilter should position the cursor at the first row of the result set or at EOF
	return xmltv_next(xmltvcursor);
}

//---------------------------------------------------------------------------
// xmltv_next
//
// Advances the virtual table cursor to the next row
//
// Arguments:
//
//	cursor		- Virtual table cusror instance

int xmltv_next(sqlite3_vtab_cursor* cursor)
{
	// Cast the provided generic cursor instance back into an xmltv_vtab_cursor instance
	xmltv_vtab_cursor* xmltvcursor = reinterpret_cast<xmltv_vtab_cursor*>(cursor);
	assert(xmltvcursor != nullptr);

	// Move the text reader to the start of the next element
	int result = xmlTextReaderRead(xmltvcursor->reader);
	while(result == 1) {

		if(xmlTextReaderNodeType(xmltvcursor->reader) == xmlReaderTypes::XML_READER_TYPE_ELEMENT) {

			bool ischannel = false;				// Flag indicating a <channel> element
			bool isprogramme = false;			// Flag indicating a <programme> element

			// Figure out if this is a <channel> or a <programme> element
			xmlChar* element = xmlTextReaderName(xmltvcursor->reader);
			if(element != nullptr) {

				ischannel = (xmlStrcmp(element, BAD_CAST("channel")) == 0);
				isprogramme = ((!ischannel) && (xmlStrcmp(element, BAD_CAST("programme")) == 0));
				xmlFree(element);
			}

			// <channel> element - invoke the callback (if present) to map the channel
			if((ischannel) && (xmltvcursor->onchannel)) {

				struct xmltv_channel channel = { 0 };
				std::vector<xmlChar*> displayNames;

				xmlNodePtr node = xmlTextReaderExpand(xmltvcursor->reader);
				if(node != nullptr) {

					xmlChar* channelid = xmlGetProp(node, BAD_CAST("id"));
					if(channelid != nullptr) {

						// The channel id attribute is passed into the callback as-is
						channel.id = reinterpret_cast<char*>(channelid);

						// The virtual channel number is in the <lcn> element
						xmlNodePtr channelnumber = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("lcn"));
						if(channelnumber != nullptr) channel.number = reinterpret_cast<char*>(xmlNodeGetContent(channelnumber));

						// Collect all of the <display-name> element values into a vector<>
						xmlTextReaderForEachChildElement(xmltvcursor->reader, BAD_CAST("display-name"), [&](xmlNodePtr node) -> void {
							displayNames.emplace_back(xmlNodeGetContent(node));
						});

						// Get the icon source URL for the channel logo
						xmlNodePtr iconsrc = xmlTextReaderGetChildElement(xmltvcursor->reader, BAD_CAST("icon"));
						if(iconsrc != nullptr) channel.iconsrc = reinterpret_cast<char*>(xmlGetProp(iconsrc, BAD_CAST("src")));
					}
				}

				// Process the <display-name> elements that are present for this channel
				//
				// [0] - GUIDENAME
				// [1] - CHANNELNUMBER GUIDENAME
				// [2] - CHANNELNUMBER ALTERNATEGUIDENAME
				// [3] - CHANNELNUMBER
				// [4] - ALTERNATEGUIDENAME
				// [5] - NETWORKNAME
				if(displayNames.size() >= 1) channel.name = reinterpret_cast<char*>(displayNames[0]);
				if(displayNames.size() >= 5) channel.altname = reinterpret_cast<char*>(displayNames[4]);
				if(displayNames.size() >= 6) channel.network = reinterpret_cast<char*>(displayNames[5]);

				// Pass the channel information back via the specified callback function
				xmltvcursor->onchannel(channel);

				// Chase down any libxml pointers passed to the callback
				if(channel.id) xmlFree(const_cast<char*>(channel.id));
				if(channel.number) xmlFree(const_cast<char*>(channel.number));
				if(channel.iconsrc) xmlFree(const_cast<char*>(channel.iconsrc));
				for(auto it : displayNames) if(it) xmlFree(it);
			}

			// <programme> element - this is the next row for the result set; break
			else if(isprogramme) break;
		}

		// Move to the next node in the XML document
		result = xmlTextReaderRead(xmltvcursor->reader);
	};

	// Unsucessful states - set end-of-file if applicable or SQLITE_INTERNAL on error
	if(result == 0) { xmltvcursor->eof = true; return SQLITE_OK; }
	else if(result == -1) return SQLITE_INTERNAL;

	// Increment the ROWID value to be returned to SQLite when asked
	++xmltvcursor->rowid;

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// xmltv_open
//
// Creates and intializes a new virtual table cursor instance
//
// Arguments:
//
//	vtab		- Virtual table instance
//	cursor		- On success contains the allocated virtual table cursor instance

int xmltv_open(sqlite3_vtab* /*vtab*/, sqlite3_vtab_cursor** cursor)
{
	// Allocate and initialize the custom virtual table cursor class
	try { *cursor = static_cast<sqlite3_vtab_cursor*>(new xmltv_vtab_cursor()); }
	catch(...) { return SQLITE_ERROR; }

	return (*cursor == nullptr) ? SQLITE_NOMEM : SQLITE_OK;
}

//---------------------------------------------------------------------------
// xmltv_rowid
//
// Retrieves the ROWID for the current virtual table cursor row
//
// Arguments:
//
//	cursor		- Virtual table cursor instance
//	rowid		- On success contains the ROWID for the current row

int xmltv_rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* rowid)
{
	// Cast the provided generic cursor instance back into an xmltv_vtab_cursor instance
	xmltv_vtab_cursor* xmltvcursor = reinterpret_cast<xmltv_vtab_cursor*>(cursor);
	assert(xmltvcursor != nullptr);

	*rowid = xmltvcursor->rowid;
	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// xmltv_time_to_w3c
//
// SQLite scalar function to convert an XMLTV time stamp into a W3C format
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void xmltv_time_to_w3c(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int				year = 0;					// calendar year
	int				month = 0;					// calendar month
	int				day = 0;					// calendar day
	int				hour = 0;					// clock hours
	int				minute = 0;					// clock minutes
	int				second = 0;					// clock seconds
	char			tzoperator = '+';			// timezone operator
	int				tzhour = 0;					// timezone hours
	int				tzminute = 0;				// timezone minutes

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// Null or zero-length input string results in null
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_null(context);

	// Attempt to scan as much of the input string as possible based on the expected format
	int parts = sscanf(str, "%04d%02d%02d%02d%02d%02d %c%02d%02d", &year, &month, &day, &hour, &minute, &second, &tzoperator, &tzhour, &tzminute);

	// YYYY-MM-DDThh:mm:ssTZD
	if(parts == 9)
		return sqlite3_result_text(context, sqlite3_mprintf("%04u-%02u-%02uT%02u:%02u:%02u%c%02u:%02u", year, month, day, hour, minute, second, tzoperator, tzhour, tzminute), -1, sqlite3_free);

	// YYYY-MM-DDThh:mm:ss
	else if(parts >= 6)
		return sqlite3_result_text(context, sqlite3_mprintf("%04u-%02u-%02uT%02u:%02u:%02u", year, month, day, hour, minute, second), -1, sqlite3_free);

	// YYYY-MM-DDThh:mm
	else if(parts >= 5)
		return sqlite3_result_text(context, sqlite3_mprintf("%04u-%02u-%02uT%02u:%02u", year, month, day, hour, minute), -1, sqlite3_free);

	// YYYY-MM-DD
	else if(parts >= 3) 
		return sqlite3_result_text(context, sqlite3_mprintf("%04u-%02u-%02u", year, month, day), -1, sqlite3_free);

	// No format match possible; return null
	return sqlite3_result_null(context);
}

//---------------------------------------------------------------------------
// xmltv_time_to_year
//
// SQLite scalar function to convert an XMLTV time stamp into an integer year
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void xmltv_time_to_year(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int				year = 0;					// calendar year

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// Null or zero-length input string results in null
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_null(context);

	// Only care about the first four digits of the XMLTV timestamp here
	if(sscanf(str, "%04d", &year) == 1) return sqlite3_result_int(context, year);

	// No format match possible; return null
	return sqlite3_result_null(context);
}

//---------------------------------------------------------------------------
// sqlite3_extension_init
//
// SQLite Extension Library entry point
//
// Arguments:
//
//	db		- SQLite database instance
//	errmsg	- On failure set to the error message (use sqlite3_malloc() to allocate)
//	api		- Pointer to the SQLite API functions

extern "C" int sqlite3_extension_init(sqlite3 *db, char** errmsg, const sqlite3_api_routines* api)
{
	SQLITE_EXTENSION_INIT2(api);

	*errmsg = nullptr;							// Initialize [out] variable

	// libcurl should be initialized prior to anything using it
	curl_global_init(CURL_GLOBAL_DEFAULT);

	// clean_filename function
	//
	int result = sqlite3_create_function_v2(db, "clean_filename", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, clean_filename, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function clean_filename"); return result; }

	// decode_channel_id function
	//
	result = sqlite3_create_function_v2(db, "decode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, decode_channel_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function decode_channel_id"); return result; }

	// decode_star_rating function
	//
	result = sqlite3_create_function_v2(db, "decode_star_rating", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, decode_star_rating, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function decode_star_rating"); return result; }

	// encode_channel_id function
	//
	result = sqlite3_create_function_v2(db, "encode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, encode_channel_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function encode_channel_id"); return result; }

	// fnv_hash function
	//
	result = sqlite3_create_function_v2(db, "fnv_hash", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, fnv_hash, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function fnv_hash"); return result; }

	// get_channel_number function
	//
	result = sqlite3_create_function_v2(db, "get_channel_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_channel_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_channel_number"); return result; }

	// get_episode_number function
	//
	result = sqlite3_create_function_v2(db, "get_episode_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_episode_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_episode_number"); return result; }

	// get_primary_genre function
	//
	result = sqlite3_create_function_v2(db, "get_primary_genre", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_primary_genre, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_primary_genre"); return result; }

	// get_recording_id function
	//
	result = sqlite3_create_function_v2(db, "get_recording_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_recording_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_recording_id"); return result; }

	// get_season_number function
	//
	result = sqlite3_create_function_v2(db, "get_season_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_season_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_season_number"); return result; }

	// json_get (1 argument; non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "json_get", 1, SQLITE_UTF8, nullptr, json_get, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function json_get(1)"); return result; }

	// json_get (2 arguments; non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "json_get", 2, SQLITE_UTF8, nullptr, json_get, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function json_get(2)"); return result; }

	// json_get (3 arguments; non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "json_get", 3, SQLITE_UTF8, nullptr, json_get, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function json_get(3)"); return result; }

	// json_get_aggregate (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "json_get_aggregate", 2, SQLITE_UTF8, nullptr, nullptr, json_get_aggregate_step, json_get_aggregate_final, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register aggregate function json_get_aggregate"); return result; }

	// uuid extension
	//
	result = sqlite3_uuid_init(db, nullptr, sqlite3_api);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register extension uuid"); return result; }

	// url_encode function
	//
	result = sqlite3_create_function_v2(db, "url_encode", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, url_encode, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function url_encode"); return result; }

	// url_remove_query_string function
	//
	result = sqlite3_create_function_v2(db, "url_remove_query_string", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, url_remove_query_string, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function url_remove_query_string"); return result; }

	// xmltv virtual table
	//
	result = sqlite3_create_module_v2(db, "xmltv", &g_xmltv_module, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register virtual table module xmltv"); return result; }

	// xmltv_time_to_w3c function
	//
	result = sqlite3_create_function_v2(db, "xmltv_time_to_w3c", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, xmltv_time_to_w3c, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function xmltv_time_to_w3c"); return result; }

	// xmltv_time_to_year function
	//
	result = sqlite3_create_function_v2(db, "xmltv_time_to_year", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, xmltv_time_to_year, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function xmltv_time_to_year"); return result; }

	// zipfile extension
	//
	result = sqlite3_zipfile_init(db, nullptr, sqlite3_api);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register extension zipfile"); return result; }

	return SQLITE_OK;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
