/****************************************************************************
 *
 *   Copyright (C) 2012 PX4 Development Team. All rights reserved.
 *   Author: Thomas Gubler <thomasgubler@student.ethz.ch>
 *           Julian Oes <joes@student.ethz.ch>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/* @file U-Blox protocol implementation */


#include "ubx.h"
#include "gps.h"
#include <sys/prctl.h>
#include <poll.h>
#include <drivers/drv_hrt.h>
#include <uORB/uORB.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include "vehicle_gps_position_old.h"
#include <mavlink/mavlink_log.h>

#define UBX_HEALTH_SUCCESS_COUNTER_LIMIT 2
#define UBX_HEALTH_FAIL_COUNTER_LIMIT 3
#define UBX_HEALTH_PROBE_COUNTER_LIMIT 4

#define UBX_BUFFER_SIZE 500

extern bool gps_mode_try_all;
extern bool gps_mode_success;
extern bool terminate_gps_thread;
extern bool gps_baud_try_all;
extern bool gps_verbose;
extern int current_gps_speed;
extern bool thread_should_exit;

pthread_mutex_t *ubx_mutex;
gps_bin_ubx_state_t *ubx_state;
enum UBX_CONFIG_STATE ubx_config_state;
static struct vehicle_gps_position_s *ubx_gps;



void ubx_decode_init(void)
{
	ubx_state->ck_a = 0;
	ubx_state->ck_b = 0;
	ubx_state->rx_count = 0;
	ubx_state->decode_state = UBX_DECODE_UNINIT;
	ubx_state->message_class = CLASS_UNKNOWN;
	ubx_state->message_id = ID_UNKNOWN;
	ubx_state->payload_size = 0;
	ubx_state->print_errors = false;
}

void ubx_checksum(uint8_t b, uint8_t *ck_a, uint8_t *ck_b)
{
	*(ck_a) = *(ck_a) + b;
	*(ck_b) = *(ck_b) + *(ck_a);
}





int ubx_parse(uint8_t b,  char *gps_rx_buffer)
{
	//printf("b=%x\n",b);
	if (ubx_state->decode_state == UBX_DECODE_UNINIT) {

		if (b == UBX_SYNC_1) {
			ubx_state->decode_state = UBX_DECODE_GOT_SYNC1;
		}

	} else if (ubx_state->decode_state == UBX_DECODE_GOT_SYNC1) {
		if (b == UBX_SYNC_2) {
			ubx_state->decode_state = UBX_DECODE_GOT_SYNC2;

		} else {
			// Second start symbol was wrong, reset state machine
			ubx_decode_init();
		}

	} else if (ubx_state->decode_state == UBX_DECODE_GOT_SYNC2) {
		// Add to checksum
		ubx_checksum(b, &(ubx_state->ck_a), &(ubx_state->ck_b));

		//check for known class
		switch (b) {
		case UBX_CLASS_ACK:
			ubx_state->decode_state = UBX_DECODE_GOT_CLASS;
			ubx_state->message_class = ACK;
			break;

		case UBX_CLASS_NAV:
			ubx_state->decode_state = UBX_DECODE_GOT_CLASS;
			ubx_state->message_class = NAV;
			break;

		case UBX_CLASS_RXM:
			ubx_state->decode_state = UBX_DECODE_GOT_CLASS;
			ubx_state->message_class = RXM;
			break;

		case UBX_CLASS_CFG:
			ubx_state->decode_state = UBX_DECODE_GOT_CLASS;
			ubx_state->message_class = CFG;
			break;
		default: //unknown class: reset state machine
			ubx_decode_init();
			break;
		}

	} else if (ubx_state->decode_state == UBX_DECODE_GOT_CLASS) {
		// Add to checksum
		ubx_checksum(b, &(ubx_state->ck_a), &(ubx_state->ck_b));

		//depending on class look for message id
		switch (ubx_state->message_class) {
		case NAV:
			switch (b) {
			case UBX_MESSAGE_NAV_POSLLH: //NAV-POSLLH: Geodetic Position Solution
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = NAV_POSLLH;
				break;

			case UBX_MESSAGE_NAV_SOL:
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = NAV_SOL;
				break;

			case UBX_MESSAGE_NAV_TIMEUTC:
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = NAV_TIMEUTC;
				break;

			case UBX_MESSAGE_NAV_DOP:
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = NAV_DOP;
				break;

			case UBX_MESSAGE_NAV_SVINFO:
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = NAV_SVINFO;
				break;

			case UBX_MESSAGE_NAV_VELNED:
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = NAV_VELNED;
				break;

			default: //unknown class: reset state machine, should not happen
				ubx_decode_init();
				break;
			}

			break;

		case RXM:
			switch (b) {
			case UBX_MESSAGE_RXM_SVSI:
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = RXM_SVSI;
				break;

			default: //unknown class: reset state machine, should not happen
				ubx_decode_init();
				break;
			}
			break;

		case CFG:
			switch (b) {
			case UBX_MESSAGE_CFG_NAV5:
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = CFG_NAV5;
				break;

			default: //unknown class: reset state machine, should not happen
				ubx_decode_init();
				break;
			}
			break;

		case ACK:
			switch (b) {
			case UBX_MESSAGE_ACK_ACK:
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = ACK_ACK;
				break;
			case UBX_MESSAGE_ACK_NAK:
				ubx_state->decode_state = UBX_DECODE_GOT_MESSAGEID;
				ubx_state->message_id = ACK_NAK;
				break;
			default: //unknown class: reset state machine, should not happen
				ubx_decode_init();
				break;
			}
			break;
		default: //should not happen
			ubx_decode_init();
			break;
		}

	} else if (ubx_state->decode_state == UBX_DECODE_GOT_MESSAGEID) {
		// Add to checksum
		ubx_checksum(b, &(ubx_state->ck_a), &(ubx_state->ck_b));

		ubx_state->payload_size = b;
		ubx_state->decode_state = UBX_DECODE_GOT_LENGTH1;

	} else if (ubx_state->decode_state == UBX_DECODE_GOT_LENGTH1) {
		// Add to checksum
		ubx_checksum(b, &(ubx_state->ck_a), &(ubx_state->ck_b));

		ubx_state->payload_size += b << 8;
		ubx_state->decode_state = UBX_DECODE_GOT_LENGTH2;

	} else if (ubx_state->decode_state == UBX_DECODE_GOT_LENGTH2) {
		uint8_t ret = 0;

		// Add to checksum if not yet at checksum byte
		if (ubx_state->rx_count < ubx_state->payload_size) ubx_checksum(b, &(ubx_state->ck_a), &(ubx_state->ck_b));

		// Fill packet buffer
		gps_rx_buffer[ubx_state->rx_count] = b;

		//if whole payload + checksum is in buffer:
		if (ubx_state->rx_count >= ubx_state->payload_size + 1) {
			//convert to correct struct
			switch (ubx_state->message_id) { //this enum is unique for all ids --> no need to check the class
			case NAV_POSLLH: {
//					printf("GOT NAV_POSLLH MESSAGE\n");
					gps_bin_nav_posllh_packet_t *packet = (gps_bin_nav_posllh_packet_t *) gps_rx_buffer;

					//Check if checksum is valid and the store the gps information
					if (ubx_state->ck_a == packet->ck_a && ubx_state->ck_b == packet->ck_b) {
						ubx_gps->lat = packet->lat;
						ubx_gps->lon = packet->lon;
						ubx_gps->alt = packet->height_msl;

						ubx_gps->counter_pos_valid++;

						ubx_gps->timestamp = hrt_absolute_time();
						ubx_gps->counter++;

						//pthread_mutex_lock(ubx_mutex);
						ubx_state->last_message_timestamps[NAV_POSLLH - 1] = hrt_absolute_time();
						//pthread_mutex_unlock(ubx_mutex);
						ret = 1;

					} else {
						if (gps_verbose) printf("[gps] NAV_POSLLH: checksum invalid\n");

						ret = 0;
					}

					// Reset state machine to decode next packet
					ubx_decode_init();
					return ret;

					break;
				}

			case NAV_SOL: {
//					printf("GOT NAV_SOL MESSAGE\n");
					gps_bin_nav_sol_packet_t *packet = (gps_bin_nav_sol_packet_t *) gps_rx_buffer;

					//Check if checksum is valid and the store the gps information
					if (ubx_state->ck_a == packet->ck_a && ubx_state->ck_b == packet->ck_b) {

						ubx_gps->fix_type = packet->gpsFix;

						ubx_gps->timestamp = hrt_absolute_time();
						ubx_gps->counter++;
						ubx_gps->s_variance = packet->sAcc;
						ubx_gps->p_variance = packet->pAcc;

						//pthread_mutex_lock(ubx_mutex);
						ubx_state->last_message_timestamps[NAV_SOL - 1] = hrt_absolute_time();
						//pthread_mutex_unlock(ubx_mutex);
						ret = 1;

					} else {
						if (gps_verbose) printf("[gps] NAV_SOL: checksum invalid\n");

						ret = 0;
					}

					// Reset state machine to decode next packet
					ubx_decode_init();
					return ret;

					break;
				}

			case NAV_DOP: {
//					printf("GOT NAV_DOP MESSAGE\n");
					gps_bin_nav_dop_packet_t *packet = (gps_bin_nav_dop_packet_t *) gps_rx_buffer;

					//Check if checksum is valid and the store the gps information
					if (ubx_state->ck_a == packet->ck_a && ubx_state->ck_b == packet->ck_b) {

						ubx_gps->eph =  packet->hDOP;
						ubx_gps->epv =  packet->vDOP;

						ubx_gps->timestamp = hrt_absolute_time();
						ubx_gps->counter++;


						//pthread_mutex_lock(ubx_mutex);
						ubx_state->last_message_timestamps[NAV_DOP - 1] = hrt_absolute_time();
						//pthread_mutex_unlock(ubx_mutex);
						ret = 1;

					} else {
						if (gps_verbose) printf("[gps] NAV_DOP: checksum invalid\n");

						ret = 0;
					}

					// Reset state machine to decode next packet
					ubx_decode_init();
					return ret;

					break;
				}

			case NAV_TIMEUTC: {
//					printf("GOT NAV_TIMEUTC MESSAGE\n");
					gps_bin_nav_timeutc_packet_t *packet = (gps_bin_nav_timeutc_packet_t *) gps_rx_buffer;

					//Check if checksum is valid and the store the gps information
					if (ubx_state->ck_a == packet->ck_a && ubx_state->ck_b == packet->ck_b) {
						//convert to unix timestamp
						struct tm timeinfo;
						timeinfo.tm_year = packet->year - 1900;
						timeinfo.tm_mon = packet->month - 1;
						timeinfo.tm_mday = packet->day;
						timeinfo.tm_hour = packet->hour;
						timeinfo.tm_min = packet->min;
						timeinfo.tm_sec = packet->sec;

						time_t epoch = mktime(&timeinfo);

//						printf("%d.%d.%d %d:%d:%d:%d\n", timeinfo.tm_year, timeinfo.tm_mon, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, packet->time_nanoseconds);



						ubx_gps->time_gps_usec = (uint64_t)epoch * 1000000; //TODO: test this
						ubx_gps->time_gps_usec += (uint64_t)(packet->time_nanoseconds * 1e-3f);

						ubx_gps->timestamp = hrt_absolute_time();
						ubx_gps->counter++;


						//pthread_mutex_lock(ubx_mutex);
						ubx_state->last_message_timestamps[NAV_TIMEUTC - 1] = hrt_absolute_time();
						//pthread_mutex_unlock(ubx_mutex);
						ret = 1;

					} else {
						if (gps_verbose) printf("\t[gps] NAV_TIMEUTC: checksum invalid\n");

						ret = 0;
					}

					// Reset state machine to decode next packet
					ubx_decode_init();
					return ret;

					break;
				}

			case NAV_SVINFO: {
//					printf("GOT NAV_SVINFO MESSAGE\n");

					//this is a more complicated message: the length depends on the number of satellites. This number is extracted from the first part of the message
					const int length_part1 = 8;
					char gps_rx_buffer_part1[length_part1];
					memcpy(gps_rx_buffer_part1, gps_rx_buffer, length_part1);
					gps_bin_nav_svinfo_part1_packet_t *packet_part1 = (gps_bin_nav_svinfo_part1_packet_t *) gps_rx_buffer_part1;

					//read checksum
					const int length_part3 = 2;
					char gps_rx_buffer_part3[length_part3];
					memcpy(gps_rx_buffer_part3, &(gps_rx_buffer[ubx_state->rx_count - 1]), length_part3);
					gps_bin_nav_svinfo_part3_packet_t *packet_part3 = (gps_bin_nav_svinfo_part3_packet_t *) gps_rx_buffer_part3;

					//Check if checksum is valid and then store the gps information
					if (ubx_state->ck_a == packet_part3->ck_a && ubx_state->ck_b == packet_part3->ck_b) {
						//definitions needed to read numCh elements from the buffer:
						const int length_part2 = 12;
						gps_bin_nav_svinfo_part2_packet_t *packet_part2;
						char gps_rx_buffer_part2[length_part2]; //for temporal storage


						int i;

						for (i = 0; i < packet_part1->numCh; i++) { //for each channel

							/* Get satellite information from the buffer */

							memcpy(gps_rx_buffer_part2, &(gps_rx_buffer[length_part1 + i * length_part2]), length_part2);
							packet_part2 = (gps_bin_nav_svinfo_part2_packet_t *) gps_rx_buffer_part2;


							/* Write satellite information in the global storage */

							ubx_gps->satellite_prn[i] = packet_part2->svid;

							//if satellite information is healthy store the data
							uint8_t unhealthy = packet_part2->flags & 1 << 4; //flags is a bitfield

							if (!unhealthy) {

								if ((packet_part2->flags) & 1) { //flags is a bitfield
									ubx_gps->satellite_used[i] = 1;

								} else {
									ubx_gps->satellite_used[i] = 0;
								}

								ubx_gps->satellite_snr[i] = packet_part2->cno;
								ubx_gps->satellite_elevation[i] = (uint8_t)(packet_part2->elev);
								ubx_gps->satellite_azimuth[i] = (uint8_t)((float)packet_part2->azim * 255.0f / 360.0f);

							} else {
								ubx_gps->satellite_used[i] = 0;
								ubx_gps->satellite_snr[i] = 0;
								ubx_gps->satellite_elevation[i] = 0;
								ubx_gps->satellite_azimuth[i] = 0;
							}

						}

						for (i = packet_part1->numCh; i < 20; i++) { //these channels are unused
							/* Unused channels have to be set to zero for e.g. MAVLink */
							ubx_gps->satellite_prn[i] = 0;
							ubx_gps->satellite_used[i] = 0;
							ubx_gps->satellite_snr[i] = 0;
							ubx_gps->satellite_elevation[i] = 0;
							ubx_gps->satellite_azimuth[i] = 0;
						}

						/* set flag if any sat info is available */
						if (!packet_part1->numCh > 0) {
							ubx_gps->satellite_info_available = 1;

						} else {
							ubx_gps->satellite_info_available = 0;
						}

						ubx_gps->timestamp = hrt_absolute_time();
						ubx_gps->counter++;


						//pthread_mutex_lock(ubx_mutex);
						ubx_state->last_message_timestamps[NAV_SVINFO - 1] = hrt_absolute_time();
						//pthread_mutex_unlock(ubx_mutex);
						ret = 1;

					} else {
						if (gps_verbose) printf("\t[gps] NAV_SVINFO: checksum invalid\n");

						ret = 0;
					}

					// Reset state machine to decode next packet
					ubx_decode_init();
					return ret;

					break;
				}

			case NAV_VELNED: {
//					printf("GOT NAV_VELNED MESSAGE\n");
					gps_bin_nav_velned_packet_t *packet = (gps_bin_nav_velned_packet_t *) gps_rx_buffer;

					//Check if checksum is valid and the store the gps information
					if (ubx_state->ck_a == packet->ck_a && ubx_state->ck_b == packet->ck_b) {

						ubx_gps->vel = (uint16_t)packet->speed;
						ubx_gps->vel_n = packet->velN / 100.0f;
						ubx_gps->vel_e = packet->velE / 100.0f;
						ubx_gps->vel_d = packet->velD / 100.0f;
						ubx_gps->vel_ned_valid = true;
						ubx_gps->cog = (uint16_t)((float)(packet->heading) * 1e-3f);

						ubx_gps->timestamp = hrt_absolute_time();
						ubx_gps->counter++;


						//pthread_mutex_lock(ubx_mutex);
						ubx_state->last_message_timestamps[NAV_VELNED - 1] = hrt_absolute_time();
						//pthread_mutex_unlock(ubx_mutex);
						ret = 1;

					} else {
						if (gps_verbose) printf("[gps] NAV_VELNED: checksum invalid\n");

						ret = 0;
					}

					// Reset state machine to decode next packet
					ubx_decode_init();
					return ret;

					break;
				}

			case RXM_SVSI: {
//					printf("GOT RXM_SVSI MESSAGE\n");
					const int length_part1 = 7;
					char gps_rx_buffer_part1[length_part1];
					memcpy(gps_rx_buffer_part1, gps_rx_buffer, length_part1);
					gps_bin_rxm_svsi_packet_t *packet = (gps_bin_rxm_svsi_packet_t *) gps_rx_buffer_part1;

					//Check if checksum is valid and the store the gps information
					if (ubx_state->ck_a == gps_rx_buffer[ubx_state->rx_count - 1] && ubx_state->ck_b == gps_rx_buffer[ubx_state->rx_count]) {

						ubx_gps->satellites_visible = packet->numVis;

						ubx_gps->timestamp = hrt_absolute_time();
						ubx_gps->counter++;


						//pthread_mutex_lock(ubx_mutex);
						ubx_state->last_message_timestamps[RXM_SVSI - 1] = hrt_absolute_time();
						//pthread_mutex_unlock(ubx_mutex);
						ret = 1;

					} else {
						if (gps_verbose) printf("[gps] RXM_SVSI: checksum invalid\n");

						ret = 0;
					}

					// Reset state machine to decode next packet
					ubx_decode_init();
					return ret;

					break;
				}

			case ACK_ACK: {
//				printf("GOT ACK_ACK\n");
				gps_bin_ack_ack_packet_t *packet = (gps_bin_ack_ack_packet_t *) gps_rx_buffer;

				//Check if checksum is valid
				if (ubx_state->ck_a == packet->ck_a && ubx_state->ck_b == packet->ck_b) {

					switch (ubx_config_state) {
						case UBX_CONFIG_STATE_PRT:
							if (packet->clsID == UBX_CLASS_CFG && packet->msgID == UBX_MESSAGE_CFG_PRT)
								ubx_config_state++;
							break;
						case UBX_CONFIG_STATE_NAV5:
							if (packet->clsID == UBX_CLASS_CFG && packet->msgID == UBX_MESSAGE_CFG_NAV5)
								ubx_config_state++;
							break;
						case UBX_CONFIG_STATE_MSG_NAV_POSLLH:
						case UBX_CONFIG_STATE_MSG_NAV_TIMEUTC:
						case UBX_CONFIG_STATE_MSG_NAV_DOP:
						case UBX_CONFIG_STATE_MSG_NAV_SVINFO:
						case UBX_CONFIG_STATE_MSG_NAV_SOL:
						case UBX_CONFIG_STATE_MSG_NAV_VELNED:
						case UBX_CONFIG_STATE_MSG_RXM_SVSI:
							if (packet->clsID == UBX_CLASS_CFG && packet->msgID == UBX_MESSAGE_CFG_MSG)
								ubx_config_state++;
							break;
						default:
							break;
					}


					ret = 1;

				} else {
					if (gps_verbose) printf("[gps] ACK_ACK: checksum invalid\n");

					ret = 0;
				}

				// Reset state machine to decode next packet
				ubx_decode_init();
				return ret;

				break;
			}

			case ACK_NAK: {
//				printf("GOT ACK_NAK\n");
				gps_bin_ack_nak_packet_t *packet = (gps_bin_ack_nak_packet_t *) gps_rx_buffer;

				//Check if checksum is valid
				if (ubx_state->ck_a == packet->ck_a && ubx_state->ck_b == packet->ck_b) {

					if (gps_verbose) printf("[gps] the ubx gps returned: not acknowledged\n");
					ret = 1;

				} else {
					if (gps_verbose) printf("[gps] ACK_NAK: checksum invalid\n");

					ret = 0;
				}

				// Reset state machine to decode next packet
				ubx_decode_init();
				return ret;

				break;
			}

			default: //something went wrong
				ubx_decode_init();

				break;
			}
		}

		(ubx_state->rx_count)++;



	}


	return 0;     // no valid packet found

}

void calculate_ubx_checksum(uint8_t *message, uint8_t length)
{
	uint8_t ck_a = 0;
	uint8_t ck_b = 0;

	int i;

	for (i = 2; i < length - 2; i++) {
		ck_a = ck_a + message[i];
		ck_b = ck_b + ck_a;
	}

	message[length - 2] = ck_a;
	message[length - 1] = ck_b;
}

int configure_gps_ubx(int *fd)
{
	// only needed once like this
	const type_gps_bin_cfg_prt_packet_t cfg_prt_packet = {
		.clsID        = UBX_CLASS_CFG,
		.msgID        = UBX_MESSAGE_CFG_PRT,
		.length       = UBX_CFG_PRT_LENGTH,
		.portID       = UBX_CFG_PRT_PAYLOAD_PORTID,
		.mode         = UBX_CFG_PRT_PAYLOAD_MODE,
		.baudRate     = current_gps_speed,
		.inProtoMask  = UBX_CFG_PRT_PAYLOAD_INPROTOMASK,
		.outProtoMask = UBX_CFG_PRT_PAYLOAD_OUTPROTOMASK,
		.ck_a         = 0,
		.ck_b         = 0
	};

	// only needed once like this
	const type_gps_bin_cfg_nav5_packet_t cfg_nav5_packet = {
		.clsID        = UBX_CLASS_CFG,
		.msgID        = UBX_MESSAGE_CFG_NAV5,
		.length       = UBX_CFG_NAV5_LENGTH,
		.mask         = UBX_CFG_NAV5_PAYLOAD_MASK,
		.dynModel     = UBX_CFG_NAV5_PAYLOAD_DYNMODEL,
		.fixMode      = UBX_CFG_NAV5_PAYLOAD_FIXMODE,
		.ck_a         = 0,
		.ck_b         = 0
	};

	// this message is reusable for different configuration commands, so not const
	type_gps_bin_cfg_msg_packet cfg_msg_packet = {
		.clsID        = UBX_CLASS_CFG,
		.msgID        = UBX_MESSAGE_CFG_MSG,
		.length       = UBX_CFG_MSG_LENGTH,
		.rate         = UBX_CFG_MSG_PAYLOAD_RATE
	};

	uint64_t time_before_config = hrt_absolute_time();

	while(hrt_absolute_time() < time_before_config + UBX_CONFIG_TIMEOUT) {

//		if (gps_verbose) printf("[gps] ubx config state: %d\n", ubx_config_state);

		switch (ubx_config_state) {
			case UBX_CONFIG_STATE_PRT:
//				if (gps_verbose) printf("[gps] Configuring ubx with baudrate: %d\n", cfg_prt_packet.baudRate);

				write_config_message_ubx((uint8_t*)(&cfg_prt_packet), sizeof(cfg_prt_packet), fd);
				break;
			case UBX_CONFIG_STATE_NAV5:
				
				write_config_message_ubx((uint8_t*)(&cfg_nav5_packet), sizeof(cfg_nav5_packet), fd);
				break;
			case UBX_CONFIG_STATE_MSG_NAV_POSLLH:
				
				cfg_msg_packet.msgClass_payload = UBX_CLASS_NAV;
				cfg_msg_packet.msgID_payload = UBX_MESSAGE_NAV_POSLLH;
				write_config_message_ubx((uint8_t*)(&cfg_msg_packet), sizeof(cfg_msg_packet), fd);
				break;
			case UBX_CONFIG_STATE_MSG_NAV_TIMEUTC:
				
				cfg_msg_packet.msgClass_payload = UBX_CLASS_NAV;
				cfg_msg_packet.msgID_payload = UBX_MESSAGE_NAV_TIMEUTC;
				write_config_message_ubx((uint8_t*)(&cfg_msg_packet), sizeof(cfg_msg_packet), fd);
				break;
			case UBX_CONFIG_STATE_MSG_NAV_DOP:
				
				cfg_msg_packet.msgClass_payload = UBX_CLASS_NAV;
				cfg_msg_packet.msgID_payload = UBX_MESSAGE_NAV_DOP;
				write_config_message_ubx((uint8_t*)(&cfg_msg_packet), sizeof(cfg_msg_packet), fd);
				break;
			case UBX_CONFIG_STATE_MSG_NAV_SVINFO:
								
				cfg_msg_packet.msgClass_payload = UBX_CLASS_NAV;
				cfg_msg_packet.msgID_payload = UBX_MESSAGE_NAV_SVINFO;
				write_config_message_ubx((uint8_t*)(&cfg_msg_packet), sizeof(cfg_msg_packet), fd);
				break;
			case UBX_CONFIG_STATE_MSG_NAV_SOL:
				
				cfg_msg_packet.msgClass_payload = UBX_CLASS_NAV;
				cfg_msg_packet.msgID_payload = UBX_MESSAGE_NAV_SOL;
				write_config_message_ubx((uint8_t*)(&cfg_msg_packet), sizeof(cfg_msg_packet), fd);
				break;
			case UBX_CONFIG_STATE_MSG_NAV_VELNED:
				
				cfg_msg_packet.msgClass_payload = UBX_CLASS_NAV;
				cfg_msg_packet.msgID_payload = UBX_MESSAGE_NAV_VELNED;
				write_config_message_ubx((uint8_t*)(&cfg_msg_packet), sizeof(cfg_msg_packet), fd);
				break;
			case UBX_CONFIG_STATE_MSG_RXM_SVSI:
				
				cfg_msg_packet.msgClass_payload = UBX_CLASS_RXM;
				cfg_msg_packet.msgID_payload = UBX_MESSAGE_RXM_SVSI;
				write_config_message_ubx((uint8_t*)(&cfg_msg_packet), sizeof(cfg_msg_packet), fd);
				break;
			case UBX_CONFIG_STATE_CONFIGURED:
				if (gps_verbose) printf("[gps] ubx configuration finished\n");
				printf("\nGPS CONFIGURED\n");
				thread_should_exit = true;
				return OK;
				break;
			default:
				break;
		}
		usleep(10000);
	}
	if (gps_verbose) printf("[gps] ubx configuration timeout\n");
	return ERROR;
}



int read_gps_ubx(int *fd, char *gps_rx_buffer, int buffer_size)
{
	uint8_t ret = 0;
	uint8_t c;
	int rx_count = 0;
	int gpsRxOverflow = 0;

	struct pollfd fds;
	fds.fd = *fd;
	fds.events = POLLIN;

	// UBX GPS mode
	// This blocks the task until there is something on the buffer
	while (1) {
		//check if the thread should terminate
		if (terminate_gps_thread == true) {
			ret = 1;
			break;
		}
		if (poll(&fds, 1, 1000) > 0) {
			if (read(*fd, &c, 1) > 0) {

//				printf("Read %x\n",c);
				if (rx_count >= buffer_size) {
					// The buffer is already full and we haven't found a valid ubx sentence.
					// Flush the buffer and note the overflow event.
					gpsRxOverflow++;
					rx_count = 0;
					ubx_decode_init();

					if (gps_verbose) printf("[gps] Buffer full\n");

				} else {
					//gps_rx_buffer[rx_count] = c;
					rx_count++;

				}

				int msg_read = ubx_parse(c, gps_rx_buffer);

				if (msg_read > 0) {
					//printf("Found sequence\n");
					break;
				}

			} else {
				break;
			}

		} else {
			break;
		}

	}

	return ret;
}

int write_config_message_ubx(const uint8_t *message, const size_t length, const int *fd)
{
	uint8_t ck_a = 0;
	uint8_t ck_b = 0;

	unsigned int i;

	uint8_t buffer[2];
	ssize_t result_write = 0;

	//calculate and write checksum to the end
	for (i = 0; i < length-2; i++) {
		ck_a = ck_a + message[i];
		ck_b = ck_b + ck_a;
	}

	// write sync bytes first
	buffer[0] = UBX_SYNC_1;
	buffer[1] = UBX_SYNC_2;

	// write config message without the checksum
	result_write  = write(*fd, buffer, sizeof(buffer));
	result_write += write(*fd, message, length-2);

	buffer[0] = ck_a;
	buffer[1] = ck_b;

	// write the checksum
	result_write += write(*fd, buffer, sizeof(buffer));

	fsync(*fd);
	if ((unsigned int)result_write != length + 2)
		return ERROR;

	return OK;
}

void *ubx_watchdog_loop(void *args)
{
	/* Set thread name */
	prctl(PR_SET_NAME, "gps ubx watchdog", getpid());


	/* Retrieve file descriptor and thread flag */
	struct arg_struct *arguments = (struct arg_struct *)args;
	int *fd = arguments->fd_ptr;
	bool *thread_should_exit = arguments->thread_should_exit_ptr;

	ubx_config_state = UBX_CONFIG_STATE_PRT;
	/* first try to configure the GPS anyway */
	configure_gps_ubx(fd);

	/* GPS watchdog error message skip counter */

	bool ubx_healthy = false;

	uint8_t ubx_fail_count = 0;
	uint8_t ubx_success_count = 0;
	bool once_ok = false;

	int mavlink_fd = open(MAVLINK_LOG_DEVICE, 0);

	//int err_skip_counter = 0;

	while (!(*thread_should_exit)) {
		/* if some values are to old reconfigure gps */
		int i;
		pthread_mutex_lock(ubx_mutex);
		bool all_okay = true;
		uint64_t timestamp_now = hrt_absolute_time();

		for (i = 0; i < UBX_NO_OF_MESSAGES; i++) {
//			printf("timestamp_now=%llu\n", timestamp_now);
//			printf("last_message_timestamps=%llu\n", ubx_state->last_message_timestamps[i]);
			if (timestamp_now - ubx_state->last_message_timestamps[i] > UBX_WATCHDOG_CRITICAL_TIME_MICROSECONDS) {
				//printf("Warning: GPS ubx message %d not received for a long time\n", i);
				all_okay = false;
			}
		}

		pthread_mutex_unlock(ubx_mutex);

		if (!all_okay) {
			/* gps error */
			ubx_fail_count++;
//			if (err_skip_counter == 0)
//			{
//				printf("GPS Watchdog detected gps not running or having problems\n");
//				err_skip_counter = 20;
//			}
//			err_skip_counter--;
			//printf("gps_mode_try_all =%u, ubx_fail_count=%u, ubx_healthy=%u, once_ok=%u\n", gps_mode_try_all, ubx_fail_count, ubx_healthy, once_ok);


			/* If we have too many failures and another mode or baud should be tried, exit... */
			if ((gps_mode_try_all == true  || gps_baud_try_all == true) && (ubx_fail_count >= UBX_HEALTH_PROBE_COUNTER_LIMIT) && (ubx_healthy == false) && once_ok == false) {
				if (gps_verbose) printf("[gps] Connection attempt failed, no UBX module found\n");

				gps_mode_success = false;
				break;
			}

			if (ubx_healthy && ubx_fail_count == UBX_HEALTH_FAIL_COUNTER_LIMIT) {
				printf("[gps] ERROR: UBX GPS module stopped responding\n");
				// global_data_send_subsystem_info(&ubx_present_enabled);
				mavlink_log_critical(mavlink_fd, "[gps] UBX module stopped responding\n");
				ubx_healthy = false;
				ubx_success_count = 0;
			}

			/* trying to reconfigure the gps configuration */
			ubx_config_state = UBX_CONFIG_STATE_PRT;
			configure_gps_ubx(fd);
			fflush(stdout);
			sleep(1);

		} else {
			/* gps healthy */
			ubx_success_count++;
			ubx_fail_count = 0;
			once_ok = true; // XXX Should this be true on a single success, or on same criteria as ubx_healthy?

			if (!ubx_healthy && ubx_success_count == UBX_HEALTH_SUCCESS_COUNTER_LIMIT) {
				//printf("[gps] ublox UBX module status ok (baud=%d)\r\n", current_gps_speed);
				// global_data_send_subsystem_info(&ubx_present_enabled_healthy);
				mavlink_log_info(mavlink_fd, "[gps] UBX module found, status ok\n");
				ubx_healthy = true;
			}
		}
		usleep(UBX_WATCHDOG_WAIT_TIME_MICROSECONDS);
	}

	if(gps_verbose) printf("[gps] ubx loop is going to terminate\n");
	close(mavlink_fd);
	return NULL;
}

void *ubx_loop(void *args)
{
	/* Set thread name */
	prctl(PR_SET_NAME, "gps ubx read", getpid());
	
	/* Retrieve file descriptor and thread flag */
	struct arg_struct *arguments = (struct arg_struct *)args;
	int *fd = arguments->fd_ptr;
	bool *thread_should_exit = arguments->thread_should_exit_ptr;

	/* Initialize gps stuff */
	char gps_rx_buffer[UBX_BUFFER_SIZE];


	if (gps_verbose) printf("[gps] UBX protocol driver starting..\n");

	//set parameters for ubx_state

	//ubx state
	ubx_state = malloc(sizeof(gps_bin_ubx_state_t));
	//printf("gps: ubx_state created\n");
	ubx_decode_init();
	ubx_state->print_errors = false;


	/* set parameters for ubx */

	struct vehicle_gps_position_s ubx_gps_d = {.counter = 0};

	ubx_gps = &ubx_gps_d;

	orb_advert_t gps_pub = orb_advertise(ORB_ID(vehicle_gps_position), &ubx_gps);

	while (!(*thread_should_exit)) {
		/* Parse a message from the gps receiver */
		if (0 == read_gps_ubx(fd, gps_rx_buffer, UBX_BUFFER_SIZE)) {
			/* publish new GPS position */
			orb_publish(ORB_ID(vehicle_gps_position), gps_pub, ubx_gps);

		} else {
			/* de-advertise */
			close(gps_pub);
			break;
		}
	}

	if(gps_verbose) printf("[gps] ubx read is going to terminate\n");
	close(gps_pub);
	return NULL;

}
