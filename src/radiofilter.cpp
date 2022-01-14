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

#include "stdafx.h"
#include "radiofilter.h"

#pragma warning(push, 4)

// radiofilter::MPEGTS_PACKET_LENGTH (static)
//
// Length of a single mpeg-ts data packet
size_t const radiofilter::MPEGTS_PACKET_LENGTH = 188;

//---------------------------------------------------------------------------
// crc32_mpeg2_table (local)
//
// Lookup table for the crc32_mpeg2 function
//
// https://gist.github.com/Miliox/b86b60b9755faf3bd7cf
// Emiliano Firmino

static uint32_t crc32_mpeg2_table[256] = {

	0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9, 0x130476dc, 0x17c56b6b,
	0x1a864db2, 0x1e475005, 0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
	0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd, 0x4c11db70, 0x48d0c6c7,
	0x4593e01e, 0x4152fda9, 0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75,
	0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011, 0x791d4014, 0x7ddc5da3,
	0x709f7b7a, 0x745e66cd, 0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95609039,
	0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5, 0xbe2b5b58, 0xbaea46ef,
	0xb7a96036, 0xb3687d81, 0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c0b5d,
	0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49, 0xc7361b4c, 0xc3f706fb,
	0xceb42022, 0xca753d95, 0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
	0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d, 0x34867077, 0x30476dc0,
	0x3d044b19, 0x39c556ae, 0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
	0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16, 0x018aeb13, 0x054bf6a4,
	0x0808d07d, 0x0cc9cdca, 0x7897ab07, 0x7c56b6b0, 0x71159069, 0x75d48dde,
	0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02, 0x5e9f46bf, 0x5a5e5b08,
	0x571d7dd1, 0x53dc6066, 0x4d9b3063, 0x495a2dd4, 0x44190b0d, 0x40d816ba,
	0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e, 0xbfa1b04b, 0xbb60adfc,
	0xb6238b25, 0xb2e29692, 0x8aad2b2f, 0x8e6c3698, 0x832f1041, 0x87ee0df6,
	0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a, 0xe0b41de7, 0xe4750050,
	0xe9362689, 0xedf73b3e, 0xf3b06b3b, 0xf771768c, 0xfa325055, 0xfef34de2,
	0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686, 0xd5b88683, 0xd1799b34,
	0xdc3abded, 0xd8fba05a, 0x690ce0ee, 0x6dcdfd59, 0x608edb80, 0x644fc637,
	0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb, 0x4f040d56, 0x4bc510e1,
	0x46863638, 0x42472b8f, 0x5c007b8a, 0x58c1663d, 0x558240e4, 0x51435d53,
	0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47, 0x36194d42, 0x32d850f5,
	0x3f9b762c, 0x3b5a6b9b, 0x0315d626, 0x07d4cb91, 0x0a97ed48, 0x0e56f0ff,
	0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623, 0xf12f560e, 0xf5ee4bb9,
	0xf8ad6d60, 0xfc6c70d7, 0xe22b20d2, 0xe6ea3d65, 0xeba91bbc, 0xef68060b,
	0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f, 0xc423cd6a, 0xc0e2d0dd,
	0xcda1f604, 0xc960ebb3, 0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610, 0xb07daba7,
	0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b, 0x9b3660c6, 0x9ff77d71,
	0x92b45ba8, 0x9675461f, 0x8832161a, 0x8cf30bad, 0x81b02d74, 0x857130c3,
	0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640, 0x4e8ee645, 0x4a4ffbf2,
	0x470cdd2b, 0x43cdc09c, 0x7b827d21, 0x7f436096, 0x7200464f, 0x76c15bf8,
	0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24, 0x119b4be9, 0x155a565e,
	0x18197087, 0x1cd86d30, 0x029f3d35, 0x065e2082, 0x0b1d065b, 0x0fdc1bec,
	0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088, 0x2497d08d, 0x2056cd3a,
	0x2d15ebe3, 0x29d4f654, 0xc5a92679, 0xc1683bce, 0xcc2b1d17, 0xc8ea00a0,
	0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c, 0xe3a1cbc1, 0xe760d676,
	0xea23f0af, 0xeee2ed18, 0xf0a5bd1d, 0xf464a0aa, 0xf9278673, 0xfde69bc4,
	0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0, 0x9abc8bd5, 0x9e7d9662,
	0x933eb0bb, 0x97ffad0c, 0xafb010b1, 0xab710d06, 0xa6322bdf, 0xa2f33668,
	0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4
};

//---------------------------------------------------------------------------
// crc32_mpeg2 (local)
//
// Calculates a CRC32/MPEG2 value for the specified byte range
//
// https://gist.github.com/Miliox/b86b60b9755faf3bd7cf
// Emiliano Firmino

static uint32_t crc32_mpeg2(uint8_t const* data, size_t length)
{
	uint32_t crc = 0xFFFFFFFF;

	for(size_t index = 0; index < length; index++)
		crc = (crc << 8) ^ crc32_mpeg2_table[((crc >> 24) ^ *data++) & 0xFF];

	return crc;
}

//---------------------------------------------------------------------------
// null_packet (local)
//
// MPEG-TS null data packet

static uint8_t null_packet[188] = {

	0x47, 0x1F, 0xFF, 0x10, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

//---------------------------------------------------------------------------
// read_be8 (inline)
//
// Reads a big-endian 8 bit value from memory
//
// Arguments:
//
//	ptr		- Pointer to the data to be read

inline uint8_t read_be8(uint8_t const* ptr)
{
	return *ptr;
}

//---------------------------------------------------------------------------
// read_be16 (inline)
//
// Reads a big-endian 16 bit value from memory
//
// Arguments:
//
//	ptr		- Pointer to the data to be read

inline uint16_t read_be16(uint8_t const* ptr)
{
	return ntohs(*reinterpret_cast<uint16_t const*>(ptr));
}

//---------------------------------------------------------------------------
// read_be32 (inline)
//
// Reads a big-endian 32 bit value from memory
//
// Arguments:
//
//	ptr		- Pointer to the data to be read

inline uint32_t read_be32(uint8_t const* ptr)
{
	return ntohl(*reinterpret_cast<uint32_t const*>(ptr));
}

//---------------------------------------------------------------------------
// write_be8 (inline)
//
// Writes a big-endian 8 bit value into memory
//
// Arguments:
//
//	val		- Value to be written
//	ptr		- Pointer to the location to write the data

inline void write_be8(uint8_t val, uint8_t* ptr)
{
	*ptr = val;
}

//---------------------------------------------------------------------------
// write_be16 (inline)
//
// Writes a big-endian 16 bit value into memory
//
// Arguments:
//
//	val		- Value to be written
//	ptr		- Pointer to the location to write the data

inline void write_be16(uint16_t val, uint8_t* ptr)
{
	*reinterpret_cast<uint16_t*>(ptr) = htons(val);
}

//---------------------------------------------------------------------------
// write_be32 (inline)
//
// Writes a big-endian 32 bit value into memory
//
// Arguments:
//
//	val		- Value to be written
//	ptr		- Pointer to the location to write the data

inline void write_be32(uint32_t val, uint8_t* ptr)
{
	*reinterpret_cast<uint32_t*>(ptr) = htonl(val);
}

//---------------------------------------------------------------------------
// radiofilter Constructor (private)
//
// Arguments:
//
//	basestream		- Underlying pvrstream instance to wrap

radiofilter::radiofilter(std::unique_ptr<pvrstream> basestream) : m_basestream(std::move(basestream))
{
	assert(m_basestream);
}

//---------------------------------------------------------------------------
// radiofilter Destructor

radiofilter::~radiofilter()
{
	close();
}

//---------------------------------------------------------------------------
// radiofilter::canseek
//
// Gets a flag indicating if the stream allows seek operations
//
// Arguments:
//
//	NONE

bool radiofilter::canseek(void) const
{
	assert(m_basestream);
	return m_basestream->canseek();
}

//---------------------------------------------------------------------------
// radiofilter::close
//
// Closes the stream
//
// Arguments:
//
//	NONE

void radiofilter::close(void)
{
	assert(m_basestream);
	m_basestream->close();
}

//---------------------------------------------------------------------------
// radiofilter::create (static)
//
// Factory method, creates a new radiofilter instance
//
// Arguments:
//
//	basestream		- Underlying pvrstream instance to wrap

std::unique_ptr<radiofilter> radiofilter::create(std::unique_ptr<pvrstream> basestream)
{
	return std::unique_ptr<radiofilter>(new radiofilter(std::move(basestream)));
}

//---------------------------------------------------------------------------
// radiofilter::filter_packets (private)
//
// Implements the transport stream packet filter
//
// Arguments:
//
//	buffer		- Pointer to the mpeg-ts packets to filter
//	count		- Number of mpeg-ts packets provided in the buffer

void radiofilter::filter_packets(uint8_t* buffer, size_t count)
{
	// The packet filter can be disabled completely for a stream if the
	// MPEG-TS packets become misaligned; leaving it enabled might trash things
	if(!m_enablefilter) return;

	// The underlying stream may not always return aligned starting buffer positions
	// due to seek requests, but it is supposed to always end the buffer aligned
	size_t offset = (count % MPEGTS_PACKET_LENGTH);
	if(offset > 0) {

		buffer += offset;			// Move past the partial packet
		count -= offset;			// Reduce the number of bytes available
	}

	// Ensure packet alignment and skip filtering if there is less than one packet
	assert((count % MPEGTS_PACKET_LENGTH) == 0);
	if(count < MPEGTS_PACKET_LENGTH) return;

	// Iterate over all of the packets provided in the buffer
	for(size_t index = 0; index < count; index += MPEGTS_PACKET_LENGTH) {

		// Set up the pointer to the start of the packet and a working pointer
		uint8_t* packet = &buffer[index];
		uint8_t* current = packet;

		// Read relevant values from the transport stream header
		uint32_t ts_header = read_be32(current);
		uint8_t sync = (ts_header & 0xFF000000) >> 24;
		bool pusi = (ts_header & 0x00400000) == 0x00400000;
		uint16_t pid = static_cast<uint16_t>((ts_header & 0x001FFF00) >> 8);
		bool adaptation = (ts_header & 0x00000020) == 0x00000020;
		bool payload = (ts_header & 0x00000010) == 0x00000010;

		// If the sync byte isn't 0x47, this either isn't an MPEG-TS stream or the packets
		// have become misaligned.  In either case the packet filter must be disabled
		assert(sync == 0x47);
		if(sync != 0x47) { m_enablefilter = false; return; }

		// Move the pointer beyond the TS header and any adaptation bytes
		current += 4U;
		if(adaptation) { current += read_be8(current); }

		// Program Assocation Table (PAT)
		//
		if((pid == 0x0000) && (payload)) {

			// Align the payload using the pointer provided when pusi is set
			if(pusi) current += read_be8(current) + 1U;

			// Watch out for a TABLEID of 0xFF, this indicates that the remainder
			// of the packet is just stuffed with 0xFF and nothing useful is here
			if(read_be8(current) == 0xFF) continue;

			// Get the first and last section indices and skip to the section data
			uint8_t firstsection = read_be8(current + 6U);
			uint8_t lastsection = read_be8(current + 7U);
			current += 8U;

			// Iterate over all the sections and add the PMT program ids to the set<>
			for(uint8_t section = firstsection; section <= lastsection; section++) {

				uint16_t pmt_program = read_be16(current);
				if(pmt_program != 0) m_pmtpids.insert(read_be16(current + 2U) & 0x1FFF);

				current += 4U;				// Move to the next section
			}
		}

		// Program Map Table (PMT)
		//
		else if((pusi) && (payload) && (m_pmtpids.find(pid) != m_pmtpids.end())) {
			
			uint8_t* pointer = current;			// Get address of current pointer
			current += (*pointer + 1U);			// Align offset with the pointer

			// There may be multiple tables in the PMT PID like 0xC0 (SCTE Program Information Message);
			// iterate over each table to locate 0x02 (Program Map Table) until 0xFF is located
			uint8_t tableid = read_be8(current);
			while(tableid != 0xFF) {

				// 0x02 = Program Map Table
				if(tableid == 0x02) {

					// Get the overall section and table lengths
					uint16_t sectionlength = read_be16(current + 1U) & 0x0FFF;
					size_t tablelength = static_cast<size_t>(3U) + sectionlength - 4U;

					// Get the length of the N-loop descriptors and ignore them
					uint16_t descriptorslen = (read_be16(current + 10U) & 0x03FF);
					pointer = current + 12U + descriptorslen;

					// Now come the stream descriptors; these are important
					while(pointer < (current + tablelength)) {

						uint8_t streamtype = read_be8(pointer);
						uint16_t streampid = read_be16(pointer + 1U) & 0x1FFF;
						uint16_t esinfolen = read_be16(pointer + 3U) & 0x03FF;

						// There are a number of VIDEO stream descriptors recognized by ffmpeg; this
						// list may not be exhaustive and should be kept up to date with ffmpeg source 
						// file libavformat/mpegts.c (search for "AVMEDIA_TYPE_VIDEO")
						switch(streamtype) {

							case 0x01:			// AV_CODEC_ID_MPEG2VIDEO
							case 0x02:			// AV_CODEC_ID_MPEG2VIDEO
							case 0x10:			// AV_CODEC_ID_MPEG4
							case 0x1B:			// AV_CODEC_ID_H264
							case 0x20:			// AV_CODEC_ID_H264
							case 0x21:			// AV_CODEC_ID_JPEG2000
							case 0x24:			// AV_CODEC_ID_HEVC
							case 0x42:			// AV_CODEC_ID_CAVS
							case 0xD1:			// AV_CODEC_ID_DIRAC
							case 0xD2:			// AV_CODEC_ID_AVS2
							case 0xEA:			// AV_CODEC_ID_VC1
							{
								m_videopids.insert(streampid);

								// Set the stream to a forbidden id and destroy any ES info
								*pointer = 0xFF;
								if(esinfolen > 0) memset(pointer + 5U, 0xFF, esinfolen);
							}
						}

						pointer += (5U + esinfolen);
					}

					// Recalcuate and rewrite the CRC for the PMT table after it's been modified
					write_be32(crc32_mpeg2(current, tablelength), current + tablelength);
				}

				// Skip to the next table in the PMT PID
				uint16_t length = read_be16(current + 1U) & 0x3FF;
				current += 3U + length;

				// Read the next table id from the PMT
				tableid = read_be8(current);
			}
		}

		// [VIDEO] Packetized Elementary Stream (PES)
		//
		else if(m_videopids.find(pid) != m_videopids.end()) {

			// Replace the PES packet with a NULL packet to prevent probing ...
			memcpy(packet, null_packet, MPEGTS_PACKET_LENGTH);
		}

	}	// for(index ...
}

//---------------------------------------------------------------------------
// radiofilter::length
//
// Gets the length of the stream; or -1 if stream is real-time
//
// Arguments:
//
//	NONE

long long radiofilter::length(void) const
{
	assert(m_basestream);
	return m_basestream->length();
}

//---------------------------------------------------------------------------
// radiofilter::mediatype
//
// Gets the media type of the stream
//
// Arguments:
//
//	NONE

char const* radiofilter::mediatype(void) const
{
	assert(m_basestream);
	return m_basestream->mediatype();
}

//---------------------------------------------------------------------------
// radiofilter::position
//
// Gets the current position of the stream
//
// Arguments:
//
//	NONE

long long radiofilter::position(void) const
{
	assert(m_basestream);
	return m_basestream->position();
}

//---------------------------------------------------------------------------
// radiofilter::read
//
// Reads data from the live stream
//
// Arguments:
//
//	buffer		- Buffer to receive the live stream data
//	count		- Size of the destination buffer in bytes

size_t radiofilter::read(uint8_t* buffer, size_t count)
{
	assert(m_basestream);

	// Read the next chunk of data from the underlying stream and subsequently
	// apply the packet filter against that chunk of data ...
	size_t read = m_basestream->read(buffer, count);

	if(read > 0) filter_packets(buffer, read);
	return read;
}

//---------------------------------------------------------------------------
// radiofilter::realtime
//
// Gets a flag indicating if the stream is real-time
//
// Arguments:
//
//	NONE

bool radiofilter::realtime(void) const
{
	assert(m_basestream);
	return m_basestream->realtime();
}

//---------------------------------------------------------------------------
// radiofilter::seek
//
// Sets the stream pointer to a specific position
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

long long radiofilter::seek(long long position, int whence)
{
	assert(m_basestream);
	return m_basestream->seek(position, whence);
}

//---------------------------------------------------------------------------

#pragma warning(pop)
