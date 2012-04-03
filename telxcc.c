/*!
(c) 2011-2012 Petr Kutalek, Forers, s. r. o.: telxcc

Some portions/inspirations:
	(c) 2007 Vincent Penne, telx.c : Minimalistic Teletext subtitles decoder
	(c) 2001-2005 by dvb.matt, ProjectX java dvb decoder
	(c) Dave Chapman <dave@dchapman.com> 2003-2004, dvbtextsubs
	(c) Ralph Metzler, DVB driver, vbidecode
	(c) Jan Pantelje, submux-dvd
	(c) Ragnar Sundblad, dvbtextsubs, VDR teletext subtitles plugin
	(c) Scott T. Smith, dvdauthor
	(c) 2007 Vladimir Voroshilov <voroshil@gmail.com>, mplayer
	(c) 2001, 2002, 2003, 2004, 2007 Michael H. Schimek, libzvbi -- Error correction functions

Code contribution, bug fixes etc.:
	Laurent Debacker (https://github.com/debackerl)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.

I would like to thank:
	David Liontooth <lionteeth@cogweb.net> for providing me with Swedish and Norwegian TS samples and patient testing
	Professor Francis F Steen and his team from UCLA for contribution
	Laurent Debacker (https://github.com/debackerl) for bug fixes

telxcc conforms to ETSI 300 706 Presentation Level 1.5:
	Presentation Level 1 defines the basic Teletext page, characterised by the use of spacing attributes only
	and a limited alphanumeric and mosaics repertoire. Presentation Level 1.5 decoder responds as Level 1 but
	the character repertoire is extended via packets X/26.

Algorithm workflow:
	main (processing TS)
	process_pes_packet (processing PS)
	process_telx_packet (processing teletext stream)
	process_page (processing teletext data)

Further Documentation:
	ISO/IEC 13818-1 (Information technology - Generic coding of moving pictures and associated audio information: Systems):
		http://mumudvb.braice.net/mumudrupal/sites/default/files/iso13818-1.pdf
		http://en.wikipedia.org/wiki/MPEG_transport_stream
		http://en.wikipedia.org/wiki/Packetized_elementary_stream
		http://en.wikipedia.org/wiki/Elementary_stream

	ETSI 300 706 (Enhanced Teletext Specification):
		http://www.etsi.org/deliver/etsi_i_ets/300700_300799/300706/01_60/ets_300706e01p.pdf

	ISO/IEC 6937:2001 (Information technology — Coded graphic character set for text communication — Latin alphabet):
		http://webstore.iec.ch/preview/info_isoiec6937%7Bed3.0%7Den.pdf
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <inttypes.h>

#include "tables_hamming.h"
#include "tables_teletext.h"

// size of a TS packet in bytes
#define TS_PACKET_SIZE 188

// size of a TS packet payload in bytes
#define TS_PACKET_PAYLOAD_SIZE 184

typedef struct {
	uint8_t _clock_run_in; // not needed
	uint8_t _framing_code; // not needed, ETSI 300 706: const 0xe4
	uint8_t address[2];
	uint8_t data[40];
} teletext_packet_payload_t;

typedef struct {
	uint64_t show_timestamp; // show at timestamp (in ms)
	uint64_t hide_timestamp; // hide at timestamp (in ms)
	uint16_t text[25][40]; // 25 lines x 40 cols (1 screen/page) of wide chars
	uint8_t tainted; // 1 = text variable contains any data
} teletext_page_t;

// be verbose?
uint16_t config_verbose = 0;
#define VERBOSE if (config_verbose > 0)

// teletext page containing cc we want to filter
uint16_t config_page = 0;

// 13-bit packet ID for teletext stream
uint16_t config_tid = 0;

// time offset in seconds
double config_offset = 0;

// output <font...></font> tags?
uint8_t config_colours = 0;

// SRT frames produced
uint32_t frames_produced = 0;

// subtitle type pages bitmap
uint8_t cc_map[256] = { 0 };

// global TS PCR value
uint32_t global_timestamp = 0;

// ETS 300 706, chapter 8.2
inline uint8_t unham_8_4(uint8_t a) {
	return (UNHAM_8_4[a] & 0x0f);
}

// ETS 300 706, chapter 8.3
inline uint32_t unham_24_18(uint32_t a) {
	uint8_t B0 = a & 0xff;
	uint8_t B1 = (a >> 8) & 0xff;
	uint8_t B2 = (a >> 16) & 0xff;

	uint8_t D1_D4 = UNHAM_24_18_D1_D4[B0 >> 2];
	uint8_t D5_D11 = B1 & 0x7f;
	uint8_t D12_D18 = B2 & 0x7f;

	uint32_t d = D1_D4 | (D5_D11 << 4) | (D12_D18 << 11);
	uint8_t ABCDEF = UNHAM_24_18_PAR[0][B0] ^ UNHAM_24_18_PAR[1][B1] ^ UNHAM_24_18_PAR[2][B2];
	uint32_t r = d ^ UNHAM_24_18_ERR[ABCDEF];

	//fprintf(stderr, "> UNHAM24/18 A=%08x, R=%08x, CHECK=%08x\n", a, r, (((a & 0x04) >> 2) | ((a & 0x70) >> 3) | ((a & 0x7f00) >> 4) | ((a & 0x7f0000) >> 5)));
	return r;
}

inline void timestamp_to_srttime(uint64_t timestamp, char *buffer) {
	uint64_t p = timestamp;
	uint8_t h = p / 3600000;
	uint8_t m = p / 60000 - 60 * h;
	uint8_t s = p / 1000 - 3600 * h - 60 * m;
	uint16_t u = p - 3600000 * h - 60000 * m - 1000 * s;
	sprintf(buffer, "%02"PRIu8":%02"PRIu8":%02"PRIu8",%03"PRIu16, h, m, s, u);
}

inline void ucs2_to_utf8(char *r, uint16_t ch) {
	if (ch < 0x80) {
		r[0] = ch;
		r[1] = 0;
		r[2] = 0;
		r[3] = 0;
	}
	else if (ch < 0x800) {
		r[0] = (ch >> 6) | 0xc0;
		r[1] = (ch & 0x3f) | 0x80;
		r[2] = 0;
		r[3] = 0;
	}
	else {
		r[0] = (ch >> 12) | 0xe0;
		r[1] = ((ch >> 6) & 0x3f) | 0x80;
		r[2] = (ch & 0x3f) | 0x80;
		r[3] = 0;
	}
}

// check parity and translate any reasonable teletext character into ucs2
uint16_t telx_to_ucs2(uint8_t c) {
	if (PARITY_8[c] == 0) return 32;

	uint16_t r = c & 0x7f;
	if (r >= 32) r = G0[LATIN][r - 32];
	return r;
}

void process_page(const teletext_page_t *page_buffer) {
#ifdef DEBUG
	for (uint8_t row = 1; row < 25; row++) {
		fprintf(stdout, "DEBUG[%02u]: ", row);
		for (uint8_t col = 0; col < 40; col++) fprintf(stdout, "%3x ", page_buffer->text[row][col]);
		fprintf(stdout, "\n");
	}
	fprintf(stdout, "\n");
#endif

	// optimalization: slicing column by column -- higher probability we could find boxed area start mark sooner
	uint8_t page_is_empty = 1;
	for (uint8_t col = 0; col < 40; col++)
		for (uint8_t row = 1; row < 25; row++)
			if (page_buffer->text[row][col] == 0x0b) {
				page_is_empty = 0;
				goto page_is_empty;
			}
	page_is_empty:
	if (page_is_empty == 1) return;

	char timecode_show[24] = { 0 };
	timestamp_to_srttime(page_buffer->show_timestamp, timecode_show);
	timecode_show[12] = 0;

	char timecode_hide[24] = { 0 };
	timestamp_to_srttime(page_buffer->hide_timestamp, timecode_hide);
	timecode_hide[12] = 0;

	// print SRT frame
	//fprintf(stdout, "%"PRIu32"\r\n%s --> %s\r\n", ++frames_produced, timecode_show, timecode_hide);

	// process data
	for (uint8_t row = 1; row < 25; row++) {
		uint8_t font_tag_opened = 0;
		uint8_t in_boxed_area = 0;
		// ETS 300 706, chapter 12.2: Alpha White ("Set-After") - Start-of-row default condition.
		uint8_t foreground_color = 0x7;

		// skip empty lines
		uint8_t line_is_empty = 1;
		for (uint8_t col = 0; col < 40; col++)
			if (page_buffer->text[row][col] == 0x0b) {
				line_is_empty = 0;
				goto line_is_empty;
			}
		line_is_empty:
		if (line_is_empty == 1) continue;

		for (uint8_t col = 0; col < 40; col++) {
			uint16_t v = page_buffer->text[row][col];

			// last column -- close font tag
			if (col == 39) {
				if ((config_colours == 1) && (font_tag_opened == 1)) {
					fprintf(stdout, "</font> ");
					font_tag_opened = 0;
				}
				in_boxed_area = 0;
				continue;
			}

			// colours
			// white is default as stated in ETS 300 706, chapter 12.2
			// black is considered as white for telxcc purpose
			// telxcc writes <font/> tags only when needed
			// black(0), red, green, yellow, blue, magenta, cyan, white
			if ((v >= 0x01) && (v <= 0x07)) {
				if (config_colours == 1) {
					if (font_tag_opened == 1) {
						fprintf(stdout, "</font> ");
						font_tag_opened = 0;
					}
					if (v != foreground_color) {
						fprintf(stdout, "<font color=\"%s\">", COLOURS[v]);
						font_tag_opened = 1;
						foreground_color = v;
					}
				}
				// ETS 300 706, chapter 12.2: Unless operating in "Hold Mosaics" mode,
				// each character space occupied by a spacing attribute is displayed as a SPACE.
				else v = 32;
			}

			// boxed area start
			if (v == 0x0b) {
				in_boxed_area = 1;
				continue;
			}

			// boxed area end
			if (v == 0x0a) {
				in_boxed_area = 0;
				col = 38;
				continue;
			}

			// discard nonprintable chars
			if (v < 32) continue;

			// processing chars in boxed area
			if (in_boxed_area == 1) {
				char u[4] = {0, 0, 0, 0};
				ucs2_to_utf8(u, v);
				fprintf(stdout, "%s", u);
			}
		}
		fprintf(stdout, "\n");
	}
    // probably EMPTY LINE BETWEEN FRAMES
	// fprintf(stdout, "\r\n");
	fflush(stdout);
}

inline uint8_t magazine(uint16_t page) {
	return ((page >> 8) & 0xf);
}

void process_telx_packet(data_unit_t data_unit_id, teletext_packet_payload_t *packet, uint64_t timestamp) {
	// variable names conform to ETS 300 706, chapter 7.1.2
	uint8_t address = (unham_8_4(packet->address[1]) << 4) | unham_8_4(packet->address[0]);
	uint8_t m = address & 0x7;
	if (m == 0) m = 8;
	uint8_t y = (address >> 3) & 0x1f;

	static teletext_page_t page_buffer;
	static uint8_t receiving_data = 0;
	static uint8_t current_charset = 0;

	static transmission_mode_t transmission_mode = TRANSMISSION_MODE_SERIAL;

 	if (y == 0) {
	 	// CC map
		uint8_t i = (unham_8_4(packet->data[1]) << 4) | unham_8_4(packet->data[0]);
		uint8_t flag_subtitle = (unham_8_4(packet->data[5]) & 0x08) >> 3;
		cc_map[i] |= flag_subtitle << (m - 1);

		if ((config_page == 0) && (flag_subtitle > 0) && (i < 0xff)) {
			config_page = (m << 8) | (unham_8_4(packet->data[1]) << 4) | unham_8_4(packet->data[0]);
			fprintf(stderr, "- No teletext page specified, first received suitable page is %03x, not guaranteed\n", config_page);
		}
	}

	if ((y == 0) && (data_unit_id == DATA_UNIT_EBU_TELETEXT_SUBTITLE)) {
 		// Page number and control bits
		uint16_t page_number = (m << 8) | (unham_8_4(packet->data[1]) << 4) | unham_8_4(packet->data[0]);
		uint8_t charset = ((unham_8_4(packet->data[7]) & 0x08) | (unham_8_4(packet->data[7]) & 0x04) | (unham_8_4(packet->data[7]) & 0x02)) >> 1;
		uint8_t flag_suppress_header = unham_8_4(packet->data[6]) & 0x01;
		//uint8_t flag_inhibit_display = (unham_8_4(packet->data[6]) & 0x08) >> 3;

		// ETS 300 706, chapter 9.3.1.3:
		// When set to '1' the service is designated to be in Serial mode and the transmission of a page is terminated
		// by the next page header with a different page number.
		// When set to '0' the service is designated to be in Parallel mode and the transmission of a page is terminated
		// by the next page header with a different page number but the same magazine number.
		// The same setting shall be used for all page headers in the service.
		transmission_mode = unham_8_4(packet->data[7]) & 0x01;

		// ETS 300 706, chapter 7.2.1: Page is terminated by and excludes the next page header packet
		// having the same magazine address in parallel transmission mode, or any magazine address in serial transmission mode.
		if (page_number != config_page) {
			// OK, whole page was transmitted, however we need to wait for next subtitle frame;
			// otherwise it would be displayed only for a few ms
			receiving_data = 0;
			return;
		}

		// Now we have the begining of page transmittion; if there is page_buffer pending, process it
		if (page_buffer.tainted > 0) {
			// it would be nice, if subtitle hides on previous video frame, so we contract 40 ms (1 frame @25 fps)
			page_buffer.hide_timestamp = timestamp - 40;
			process_page(&page_buffer);
		}

		page_buffer.show_timestamp = timestamp;
		page_buffer.hide_timestamp = 0;
		memset(page_buffer.text, 0x00, sizeof(page_buffer.text));
		page_buffer.tainted = 0;
		receiving_data = 1;

		// remap current Latin G0 chars
		// TODO: refactore
		if (charset != current_charset) {
			G0[LATIN][0x23 - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 0];
			G0[LATIN][0x24 - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 1];
			G0[LATIN][0x40 - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 2];
			G0[LATIN][0x5b - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 3];
			G0[LATIN][0x5c - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 4];
			G0[LATIN][0x5d - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 5];
			G0[LATIN][0x5e - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 6];
			G0[LATIN][0x5f - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 7];
			G0[LATIN][0x60 - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 8];
			G0[LATIN][0x7b - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][ 9];
			G0[LATIN][0x7c - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][10];
			G0[LATIN][0x7d - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][11];
			G0[LATIN][0x7e - 0x20] = G0_LATIN_NATIONAL_SUBSETS[charset][12];
			current_charset = charset;
			VERBOSE fprintf(stderr, "- G0 Charset translation table remapped to G0 Latin National Subset ID %1x\n", current_charset);
		}

		// I know -- not needed; in subtitles we will never need disturbing teletext page status bar
		// displaying tv station name, current time etc.
		if (flag_suppress_header == 0) {
			for (uint8_t i = 14; i < 40; i++) page_buffer.text[y][i] = telx_to_ucs2(packet->data[i]);
		}
	}
	else if ((y >= 1) && (y <= 23) && (m == magazine(config_page))) {
		if ((transmission_mode == TRANSMISSION_MODE_SERIAL) && (data_unit_id != DATA_UNIT_EBU_TELETEXT_SUBTITLE)) return;
		if (receiving_data == 1) {
			// ETS 300 706, chapter 9.4.1: Packets X/26 at presentation Levels 1.5, 2.5, 3.5 are used for addressing
			// a character location and overwriting the existing character defined on the Level 1 page
			// ETS 300 706, annex B.2.2: Packets with Y = 26 shall be transmitted before any packets with Y = 1 to Y = 25;
			// so page_buffer.text[y][i] may already contain any character received
			// in frame number 26, skip original G0 character
			for (uint8_t i = 0; i < 40; i++) if (page_buffer.text[y][i] == 0x00) page_buffer.text[y][i] = telx_to_ucs2(packet->data[i]);
			page_buffer.tainted = 1;
		}
	}
	else if ((y == 26) && (m == magazine(config_page))) {
		if ((transmission_mode == TRANSMISSION_MODE_SERIAL) && (data_unit_id != DATA_UNIT_EBU_TELETEXT_SUBTITLE)) return;
		if (receiving_data == 1) {
			// ETS 300 706, chapter 12.3.2 (X/26 definition)
			uint8_t x26_row = 0;
			uint8_t x26_col = 0;

			uint32_t decoded[13] = { 0 };
			for (uint8_t i = 1, j = 0; i < 40; i += 3, j++) {
				decoded[j] = unham_24_18((packet->data[i + 2] << 16) | (packet->data[i + 1] << 8) | packet->data[i]);
				// invalid data
				if ((decoded[j] & 0x80000000) > 0) decoded[j] = 0;
			}

			for (uint8_t j = 0; j < 13; j++) {
				uint8_t data = (decoded[j] & 0x3f800) >> 11;
				uint8_t mode = (decoded[j] & 0x7c0) >> 6;
				uint8_t address = decoded[j] & 0x3f;
				uint8_t row_address_group = (address >= 40) && (address <= 63);

				// ETS 300 706, chapter 12.3.1, table 27: set active position
				if ((mode == 0x04) && (row_address_group == 1)) {
					x26_row = address - 40;
					if (x26_row == 0) x26_row = 24;
					x26_col = 0;
				}

				// ETS 300 706, chapter 12.3.1, table 27: termination marker
				if ((mode >= 0x11) && (mode <= 0x1f) && (row_address_group == 1)) break;

				// ETS 300 706, chapter 12.3.1, table 27: character from G2 set
				if ((mode == 0x0f) && (row_address_group == 0)) {
					x26_col = address;
					if (data > 31) page_buffer.text[x26_row][x26_col] = G2[0][data - 32];
				}

				// ETS 300 706, chapter 12.3.1, table 27: G0 character with diacritical mark
				if ((mode >= 0x11) && (mode <= 0x1f) && (row_address_group == 0)) {
					x26_col = address;

					// A - Z
					if ((data >= 65) && (data <= 90)) page_buffer.text[x26_row][x26_col] = G2_ACCENTS[mode - 0x11][data - 65];
					// a - z
					else if ((data >= 97) && (data <= 122)) page_buffer.text[x26_row][x26_col] = G2_ACCENTS[mode - 0x11][data - 71];
					// other
					else page_buffer.text[x26_row][x26_col] = telx_to_ucs2(data);
				}
			}
		}
	}
	else if (y == 28) {
		VERBOSE fprintf(stderr, "- Packet X/28 received; not yet implemented; you won't be able to use secondary language\n");
	}
	else if (y == 29) {
		VERBOSE fprintf(stderr, "- Packet M/29 received; not yet implemented; you won't be able to use secondary language\n");
	}
	else if ((y == 30) && (m == 8)) {
		// ETS 300 706, chapter 9.8: Broadcast Service Data Packets
		static uint8_t programme_title_processed = 0;
		if (programme_title_processed == 0) {
			// ETS 300 706, chapter 9.8.1: Packet 8/30 Format 1
			if (unham_8_4(packet->data[0]) < 2) {
				fprintf(stderr, "- Programme Identification Data = ");
				for (uint8_t i = 20; i < 40; i++) {
					char u[4] = {0, 0, 0, 0};
					ucs2_to_utf8(u, telx_to_ucs2(packet->data[i]));
					fprintf(stderr, "%s", u);
				}
				fprintf(stderr, "\n");

				// OMG! ETS 300 706 stores timestamp in 7 bytes in Modified Julian Day in BCD format + HH:MM:SS in BCD format
				// + timezone as 5-bit count of half-hours from GMT with 1-bit sign
				// In addition all decimals are incremented by 1 before transmission.
				uint32_t t = 0;
				// 1st step: BCD to Modified Julian Day
				t += (packet->data[10] & 0x0f) * 10000;
				t += ((packet->data[11] & 0xf0) >> 4) * 1000;
				t += (packet->data[11] & 0x0f) * 100;
				t += ((packet->data[12] & 0xf0) >> 4) * 10;
				t += (packet->data[12] & 0x0f);
				t -= 11111;
				// 2nd step: conversion Modified Julian Day to unix timestamp
				t = (t - 40587) * 86400;
				// 3rd step: add time
				t += 3600 * ( ((packet->data[13] & 0xf0) >> 4) * 10 + (packet->data[13] & 0x0f) );
				t +=   60 * ( ((packet->data[14] & 0xf0) >> 4) * 10 + (packet->data[14] & 0x0f) );
				t +=        ( ((packet->data[15] & 0xf0) >> 4) * 10 + (packet->data[15] & 0x0f) );
				t -= 40271;
				// 4th step: conversion to time_t
				time_t t0 = (time_t)t;
				// ctime output itself is \n-ended
				fprintf(stderr, "- Universal Time Co-ordinated = %s", ctime(&t0));

				VERBOSE fprintf(stderr, "- Transmission mode = %s\n", (transmission_mode == 1 ? "serial" : "parallel"));

				programme_title_processed = 1;
			}
		}
	}
	// else nothing; we do not process page related extension packets as in ETS 300 706, chapter 7.2.3
}

void process_pes_packet(uint8_t *buffer, uint16_t size) {
	if (size < 6) return;

	// Packetized Elementary Stream (PES) 32-bit start code
	uint64_t pes_prefix = (buffer[0] << 16) | (buffer[1] << 8) | buffer[2];
	uint8_t pes_stream_id = buffer[3];

	// check for PES header
	if (pes_prefix != 0x000001) return;

	// stream_id is not "Private Stream 1" (0xbd)
	if (pes_stream_id != 0xbd) return;

	// PES packet length
	// ETSI EN 301 775 V1.2.1 (2003-05) chapter 4.3: (N × 184) - 6 + 6 B header
	uint16_t pes_packet_length = 6 + ((buffer[4] << 8) | buffer[5]);
	// Can be zero. If the "PES packet length" is set to zero, the PES packet can be of any length.
	// A value of zero for the PES packet length can be used only when the PES packet payload is a video elementary stream.
	if (pes_packet_length == 6) return;

	// truncate incomplete PES packets
	if (pes_packet_length > size) pes_packet_length = size;

	uint8_t optional_pes_header_included = 0;
	uint16_t optional_pes_header_length = 0;
	// optional PES header marker bits (10.. ....)
	if ((buffer[6] & 0xc0) == 0x80) {
		optional_pes_header_included = 1;
		optional_pes_header_length = buffer[8];
	}

	static uint8_t using_pts = 255;
	if (using_pts == 255) {
		if ((optional_pes_header_included == 1) && ((buffer[7] & 0x80) > 0)) {
			using_pts = 1;
			VERBOSE fprintf(stderr, "- PID 0xbd PTS available\n");
		} else {
			using_pts = 0;
			VERBOSE fprintf(stderr, "- PID 0xbd PTS unavailable, using TS PCR\n");
		}
	}

	uint32_t t = 0;
	// If there is no PTS available, use global PCR
	if (using_pts == 0) t = global_timestamp;
	else {
		// PTS is 33 bits wide, however, timestamp in ms fits into 32 bits nicely (PTS/90)
		// presentation and decoder timestamps use the 90 KHz clock, hence PTS/90 = [ms]
		uint64_t pts = 0;
		// __MUST__ assign value to uint64_t and __THEN__ rotate left by 29 bits
		// << is defined for signed int (as in "C" spec.) and overflow occures
		pts = (buffer[9] & 0x0e);
		pts <<= 29;
		pts |= (buffer[10] << 22);
		pts |= ((buffer[11] & 0xfe) << 14);
		pts |= (buffer[12] << 7);
		pts |= ((buffer[13] & 0xfe) >> 1);
		t = pts / 90;
	}

	static int64_t delta = 0;
 	static uint32_t t0 = 0;
	static uint8_t initialized = 0;
	if (initialized == 0) {
		delta = 1000 * config_offset - t;
		t0 = t;
		initialized = 1;
	}
	if (t < t0) delta += 95443718;
	t0 = t;
	uint64_t timestamp = t + delta;

	// skip optional PES header and process each 46-byte teletext packet
	uint16_t i = 7;
	if (optional_pes_header_included) i += 3 + optional_pes_header_length;
	while (i <= pes_packet_length - 6) {
		uint8_t data_unit_id = buffer[i++];
		uint8_t data_unit_len = buffer[i++];

		if ((data_unit_id == DATA_UNIT_EBU_TELETEXT_NONSUBTITLE) || (data_unit_id == DATA_UNIT_EBU_TELETEXT_SUBTITLE)) {
			// teletext payload has always size 44 bytes
			if (data_unit_len == 0x2c) {
				// reverse endianess (via lookup table), ETS 300 706, chapter 7.1
				for (uint8_t j = 0; j < data_unit_len; j++) buffer[i + j] = REVERSE_8[buffer[i + j]];

				process_telx_packet(data_unit_id, (teletext_packet_payload_t *)&buffer[i], timestamp);
			}
		}

		i += data_unit_len;
	}
}

// graceful exit support
uint8_t exit_request = 0;

void signal_handler(int sig) {
	if ((sig == SIGINT) || (sig == SIGTERM)) {
		fprintf(stderr, "- SIGINT/SIGTERM received, performing graceful exit\n");
		exit_request = 1;
	}
}

int main(int argc, const char *argv[]) {
	fprintf(stderr, "telxcc - teletext closed captioning decoder\n");
	fprintf(stderr, "(c) Petr Kutalek <petr.kutalek@forers.com>, 2011-2012; Licensed under the GPL.\n");
	fprintf(stderr, "Please consider making a Paypal donation to support our free GNU/GPL software: http://fore.rs/donate/telxcc\n");
	fprintf(stderr, "Built on %s\n", __DATE__);
	fprintf(stderr, "\n");

	uint8_t config_bom = 1;
	uint8_t config_nonempty = 0;

	// command line params parsing
	for (uint8_t i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0) {
			fprintf(stderr, "Usage: telxcc [-h] | [-p PAGE] [-t TID] [-o OFFSET] [-n] [-1] [-c] [-v]\n");
			fprintf(stderr, "  STDIN       transport stream\n");
			fprintf(stderr, "  STDOUT      subtitles in SubRip SRT file format (UTF-8 encoded)\n");
			fprintf(stderr, "  -h          this help text\n");
			fprintf(stderr, "  -p PAGE     teletext page number carrying closed captioning (default: auto)\n");
			fprintf(stderr, "                (usually CZ=888, DE=150, SE=199, NO=777, UK=888 etc.)\n");
			fprintf(stderr, "  -t TID      transport stream PID of teletext data sub-stream (default: auto)\n");
			fprintf(stderr, "  -o OFFSET   subtitles offset in seconds (default: 0.0)\n");
			fprintf(stderr, "  -n          do not print UTF-8 BOM characters at the beginning of output\n");
			fprintf(stderr, "  -1          produce at least one (dummy) frame\n");
			fprintf(stderr, "  -c          output colour information in <font/> HTML tags\n");
			fprintf(stderr, "                (colours are supported by MPC, MPC HC, VLC, KMPlayer, VSFilter, ffdshow etc.)\n");
			fprintf(stderr, "  -v          be verbose (default: verboseness turned off, without being quiet)\n");
			fprintf(stderr, "\n");
			exit(EXIT_SUCCESS);
		}
		else if ((strcmp(argv[i], "-p") == 0) && (argc > i + 1))
			config_page = atoi(argv[++i]);
		else if ((strcmp(argv[i], "-t") == 0) && (argc > i + 1))
			config_tid = atoi(argv[++i]);
		else if ((strcmp(argv[i], "-o") == 0) && (argc > i + 1))
			config_offset = atof(argv[++i]);
		else if (strcmp(argv[i], "-n") == 0)
			config_bom = 0;
		else if (strcmp(argv[i], "-1") == 0)
			config_nonempty = 1;
		else if (strcmp(argv[i], "-c") == 0)
			config_colours = 1;
		else if (strcmp(argv[i], "-v") == 0)
			config_verbose = 1;
		else {
			fprintf(stderr, "- Unknown option %s\n", argv[i]);
			exit(EXIT_FAILURE);
		}
	}

	// endianness test; maybe not needed, however I do not have any Big Endian system so I can be sure... :-/
	{
		const uint32_t ENDIANNESS_TEST = 0xdeadbeef;
		if (*(const uint8_t *)&ENDIANNESS_TEST != 0xef) {
			fprintf(stderr, "- This application was tested only at Little Endian systems!\n");
			exit(EXIT_FAILURE);
		}
	}

	// teletext page number out of range
	if ((config_page != 0) && ((config_page < 100) || (config_page > 899))) {
		fprintf(stderr, "- Teletext page number could not be lower than 100 or higher than 899\n");
		exit(EXIT_FAILURE);
	}

	// default teletext page
	if (config_page > 0) {
		// dec to BCD, magazine pages numbers are in BCD (ETSI 300 706)
		config_page = ((config_page / 100) << 8) | (((config_page / 10) % 10) << 4) | (config_page % 10);
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	// disables flushing after CR/FL, we will flush manually whole SRT frames
	setbuf(stdout, NULL);

	// print UTF-8 BOM chars
	if (config_bom == 1) {
		fprintf(stdout, "\xef\xbb\xbf");
		fflush(stdout);
	}

	// FYI, packet counter
	uint32_t packet_counter = 0;

	// TS packet buffer
	uint8_t ts_buffer[TS_PACKET_SIZE];

	// 255 means not set yet
	uint8_t continuity_counter = 255;

	// PES packet buffer
	#define PES_BUFFER_SIZE 4096
	uint8_t pes_buffer[PES_BUFFER_SIZE] = { 0 };
	uint16_t pes_counter = 0;

	// reading input
	while ((exit_request == 0) && (fread(&ts_buffer, 1, TS_PACKET_SIZE, stdin) == TS_PACKET_SIZE)) {
    	// Transport Stream Header
		uint8_t ts_sync = ts_buffer[0];
		uint8_t ts_transport_error = (ts_buffer[1] & 0x80) >> 7;
		uint8_t ts_payload_unit_start = (ts_buffer[1] & 0x40) >> 6;
		uint8_t ts_transport_priority = (ts_buffer[1] & 0x20) >> 5;
		uint16_t ts_pid = ((ts_buffer[1] & 0x1f) << 8) | ts_buffer[2];
		//uint8_t ts_scrambling_control = (ts_buffer[3] & 0xc0) >> 6;
		uint8_t ts_adaptation_field_exists = (ts_buffer[3] & 0x20) >> 5;
		uint8_t ts_payload_exists = (ts_buffer[3] & 0x10) >> 4;
		uint8_t ts_continuity_counter = ts_buffer[3] & 0x0f;

		uint8_t af_discontinuity = 0;
		if (ts_adaptation_field_exists > 0) {
			af_discontinuity = (ts_buffer[5] & 0x80) >> 7;

			// PCR in adaptation field
			uint8_t af_pcr_exists = (ts_buffer[5] & 0x10) >> 4;
			if (af_pcr_exists > 0) {
				uint64_t pts = 0;
				pts |= (ts_buffer[6] << 25);
				pts |= (ts_buffer[7] << 17);
				pts |= (ts_buffer[8] << 9);
				pts |= (ts_buffer[9] << 1);
				pts |= (ts_buffer[10] >> 7);
				global_timestamp = pts/90;
				pts = ((ts_buffer[10] & 0x01) << 8);
				pts |= ts_buffer[11];
				global_timestamp += pts/27000;
			}
		}

		// not TS packet?
		if (ts_sync != 0x47) {
			fprintf(stderr, "- Invalid TS packet header\n");
			exit(EXIT_FAILURE);
		}

		// no payload
		if (ts_payload_exists == 0) continue;

		// PID filter
		if ((config_tid > 0) && (config_tid != ts_pid)) continue;

		// uncorrectable error?
		if (ts_transport_error > 0) {
			VERBOSE fprintf(stderr, "- Uncorrectable TS packet error (received CC %1x)\n", ts_continuity_counter);
			continue;
		}

		// Choose first suitable PID if not set
		if (config_tid == 0) {
			if ((ts_payload_unit_start > 0) && ((ts_buffer[4] == 0x00) && (ts_buffer[5] == 0x00) && (ts_buffer[6] == 0x01) && (ts_buffer[7] == 0xbd))) {
				config_tid = ts_pid;
				fprintf(stderr, "- No teletext PID specified, first received suitable stream PID is %"PRIu16" (0x%x), not guaranteed\n", config_tid, config_tid);
			}
			else continue;
		}

		// TS continuity check
		if (continuity_counter == 255) {
			continuity_counter = ts_continuity_counter;
		}
		else {
			if (af_discontinuity == 0) {
				continuity_counter = (continuity_counter + 1) % 16;
				if (ts_continuity_counter != continuity_counter) {
					VERBOSE fprintf(stderr, "- Missing TS packet, flushing pes_buffer (expected CC %1x, received CC %1x, TS discontinuity %s, TS priority %s)\n",
						continuity_counter, ts_continuity_counter, (af_discontinuity ? "YES" : "NO"), (ts_transport_priority ? "YES" : "NO"));
					pes_counter = 0;
					continuity_counter = 255;
				}
			}
		}

		// waiting for first payload_unit_start indicator
		if ((ts_payload_unit_start == 0) && (pes_counter == 0)) continue;

		// proceed with pes buffer
		if ((ts_payload_unit_start > 0) && (pes_counter > 0)) process_pes_packet(pes_buffer, pes_counter);

		// new pes frame start
		if (ts_payload_unit_start > 0) pes_counter = 0;

		// add pes data to buffer
		if (pes_counter < (PES_BUFFER_SIZE - TS_PACKET_PAYLOAD_SIZE)) {
			memcpy(&pes_buffer[pes_counter], &ts_buffer[4], TS_PACKET_PAYLOAD_SIZE);
			pes_counter += TS_PACKET_PAYLOAD_SIZE;
			packet_counter++;
		}
		else VERBOSE fprintf(stderr, "- PES packet size exceeds pes_buffer size, probably not teletext stream\n");
	}

	VERBOSE {
		if (frames_produced == 0) fprintf(stderr, "- No frames produced. CC teletext page number was probably wrong.\n");
		fprintf(stderr, "- There were some CC data carried via pages: ");
		// We ignore i = 0xff, because 0xffs are teletext ending frames
		for (uint16_t i = 0; i < 255; i++)
			for (uint8_t j = 0; j < 8; j++) {
				uint8_t v = cc_map[i] & (1 << j);
				if (v > 0) fprintf(stderr, "%03x ", ((j + 1) << 8) | i);
			}
		fprintf(stderr, "\n");
	}

	if ((frames_produced == 0) && (config_nonempty > 0)) {
		fprintf(stdout, "1\r\n00:00:00,000 --> 00:00:01,000\r\n(no closed captioning available)\r\n\r\n");
		fflush(stdout);
		frames_produced++;
	}

	fprintf(stderr, "- Done (%"PRIu32" teletext packets processed, %"PRIu32" SRT frames written)\n", packet_counter, frames_produced);
	fprintf(stderr, "\n");

	return EXIT_SUCCESS;
}
