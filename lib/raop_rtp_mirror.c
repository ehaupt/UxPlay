/*
 * Copyright (c) 2019 dsafa22, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "raop_rtp_mirror.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <netinet/tcp.h>

#include "raop.h"
#include "netutils.h"
#include "compat.h"
#include "logger.h"
#include "byteutils.h"
#include "mirror_buffer.h"
#include "stream.h"
#include "utils.h"
#include "plist/plist.h"

/* for MacOS, where SOL_TCP and TCP_KEEPIDLE are not defined */
#if !defined(SOL_TCP) && defined(IPPROTO_TCP)
#define SOL_TCP IPPROTO_TCP
#endif
#if !defined(TCP_KEEPIDLE) && defined(TCP_KEEPALIVE)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif

struct h264codec_s {
    unsigned char compatibility;
    short pps_size;
    short sps_size;
    unsigned char level;
    unsigned char number_of_pps;
    unsigned char* picture_parameter_set;
    unsigned char profile_high;
    unsigned char reserved_3_and_sps;
    unsigned char reserved_6_and_nal;
    unsigned char* sequence_parameter_set;
    unsigned char version;
};

struct raop_rtp_mirror_s {
    logger_t *logger;
    raop_callbacks_t callbacks;
    raop_ntp_t *ntp;

    /* Buffer to handle all resends */
    mirror_buffer_t *buffer;

    /* Remote address as sockaddr */
    struct sockaddr_storage remote_saddr;
    socklen_t remote_saddr_len;

    /* MUTEX LOCKED VARIABLES START */
    /* These variables only edited mutex locked */
    int running;
    int joined;

    int flush;
    thread_handle_t thread_mirror;
    mutex_handle_t run_mutex;

    /* MUTEX LOCKED VARIABLES END */
    int mirror_data_sock;

    unsigned short mirror_data_lport;

    /* SPS and PPS */
    int sps_pps_len;
    unsigned char* sps_pps;  

     /* switch for displaying client FPS data */
     uint8_t show_client_FPS_data;
};

static int
raop_rtp_parse_remote(raop_rtp_mirror_t *raop_rtp_mirror, const unsigned char *remote, int remotelen)
{
    char current[25];
    int family;
    int ret;
    assert(raop_rtp_mirror);
    if (remotelen == 4) {
        family = AF_INET;
    } else if (remotelen == 16) {
        family = AF_INET6;
    } else {
        return -1;
    }
    memset(current, 0, sizeof(current));
    sprintf(current, "%d.%d.%d.%d", remote[0], remote[1], remote[2], remote[3]);
    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror parse remote ip = %s", current);
    ret = netutils_parse_address(family, current,
                                 &raop_rtp_mirror->remote_saddr,
                                 sizeof(raop_rtp_mirror->remote_saddr));
    if (ret < 0) {
        return -1;
    }
    raop_rtp_mirror->remote_saddr_len = ret;
    return 0;
}

#define NO_FLUSH (-42)
raop_rtp_mirror_t *raop_rtp_mirror_init(logger_t *logger, raop_callbacks_t *callbacks, raop_ntp_t *ntp,
                                        const unsigned char *remote, int remotelen, const unsigned char *aeskey)
{
    raop_rtp_mirror_t *raop_rtp_mirror;

    assert(logger);
    assert(callbacks);

    raop_rtp_mirror = calloc(1, sizeof(raop_rtp_mirror_t));
    if (!raop_rtp_mirror) {
        return NULL;
    }
    raop_rtp_mirror->logger = logger;
    raop_rtp_mirror->ntp = ntp;
    raop_rtp_mirror->sps_pps_len = 0;
    raop_rtp_mirror->sps_pps = NULL;
    
    memcpy(&raop_rtp_mirror->callbacks, callbacks, sizeof(raop_callbacks_t));
    raop_rtp_mirror->buffer = mirror_buffer_init(logger, aeskey);
    if (!raop_rtp_mirror->buffer) {
        free(raop_rtp_mirror);
        return NULL;
    }
    if (raop_rtp_parse_remote(raop_rtp_mirror, remote, remotelen) < 0) {
        free(raop_rtp_mirror);
        return NULL;
    }
    raop_rtp_mirror->running = 0;
    raop_rtp_mirror->joined = 1;
    raop_rtp_mirror->flush = NO_FLUSH;

    MUTEX_CREATE(raop_rtp_mirror->run_mutex);
    return raop_rtp_mirror;
}

void
raop_rtp_init_mirror_aes(raop_rtp_mirror_t *raop_rtp_mirror, uint64_t *streamConnectionID)
{
    mirror_buffer_init_aes(raop_rtp_mirror->buffer, streamConnectionID);
}

//#define DUMP_H264

#define RAOP_PACKET_LEN 32768
/**
 * Mirror
 */
static THREAD_RETVAL
raop_rtp_mirror_thread(void *arg)
{
    raop_rtp_mirror_t *raop_rtp_mirror = arg;
    assert(raop_rtp_mirror);

    int stream_fd = -1;
    unsigned char packet[128];
    memset(packet, 0 , 128);
    unsigned char* payload = NULL;
    unsigned int readstart = 0;
    bool conn_reset = false;
    uint64_t ntp_timestamp_nal = 0;
    uint64_t ntp_timestamp_raw = 0;
    
#ifdef DUMP_H264
    // C decrypted
    FILE* file = fopen("/home/pi/Airplay.h264", "wb");
    // Encrypted source file
    FILE* file_source = fopen("/home/pi/Airplay.source", "wb");
    FILE* file_len = fopen("/home/pi/Airplay.len", "wb");
#endif

    while (1) {
        fd_set rfds;
        struct timeval tv;
        int nfds, ret;
        MUTEX_LOCK(raop_rtp_mirror->run_mutex);
        if (!raop_rtp_mirror->running) {
            MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
            break;
        }
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

        /* Set timeout value to 5ms */
        tv.tv_sec = 0;
        tv.tv_usec = 5000;

        /* Get the correct nfds value and set rfds */
        FD_ZERO(&rfds);
        if (stream_fd == -1) {
            FD_SET(raop_rtp_mirror->mirror_data_sock, &rfds);
            nfds = raop_rtp_mirror->mirror_data_sock+1;
        } else {
            FD_SET(stream_fd, &rfds);
            nfds = stream_fd+1;
        }
        ret = select(nfds, &rfds, NULL, NULL, &tv);
        if (ret == 0) {
            /* Timeout happened */
            continue;
        } else if (ret == -1) {
            logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror error in select");
            break;
        }

        if (stream_fd == -1 && FD_ISSET(raop_rtp_mirror->mirror_data_sock, &rfds)) {
            struct sockaddr_storage saddr;
            socklen_t saddrlen;
            logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror accepting client");
            saddrlen = sizeof(saddr);
            stream_fd = accept(raop_rtp_mirror->mirror_data_sock, (struct sockaddr *)&saddr, &saddrlen);
            if (stream_fd == -1) {
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror error in accept %d %s", errno, strerror(errno));
                break;
            }

            // We're calling recv for a certain amount of data, so we need a timeout
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 5000;
            if (setsockopt(stream_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror could not set stream socket timeout %d %s", errno, strerror(errno));
                break;
            }
            int option;
            option = 1;
            if (setsockopt(stream_fd, SOL_SOCKET, SO_KEEPALIVE, &option, sizeof(option)) < 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_WARNING, "raop_rtp_mirror could not set stream socket keepalive %d %s", errno, strerror(errno));
            }
            option = 60;
            if (setsockopt(stream_fd, SOL_TCP, TCP_KEEPIDLE, &option, sizeof(option)) < 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_WARNING, "raop_rtp_mirror could not set stream socket keepalive time %d %s", errno, strerror(errno));
            }
            option = 10;
            if (setsockopt(stream_fd, SOL_TCP, TCP_KEEPINTVL, &option, sizeof(option)) < 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_WARNING, "raop_rtp_mirror could not set stream socket keepalive interval %d %s", errno, strerror(errno));
            }
            option = 6;
            if (setsockopt(stream_fd, SOL_TCP, TCP_KEEPCNT, &option, sizeof(option)) < 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_WARNING, "raop_rtp_mirror could not set stream socket keepalive probes %d %s", errno, strerror(errno));
            }
            readstart = 0;
        }

        if (stream_fd != -1 && FD_ISSET(stream_fd, &rfds)) {

            // The first 128 bytes are some kind of header for the payload that follows
            while (payload == NULL && readstart < 128) {
                ret = recv(stream_fd, packet + readstart, 128 - readstart, 0);
                if (ret <= 0) break;
                readstart = readstart + ret;
            }

            if (payload == NULL && ret == 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror tcp socket closed");
                FD_CLR(stream_fd, &rfds);
                stream_fd = -1;
                continue;
            } else if (payload == NULL && ret == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // Timeouts can happen even if the connection is fine
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror error  in header recv: %d %s", errno, strerror(errno));
                if (errno == ECONNRESET) conn_reset = true;; 
                break;
            }

            int payload_size = byteutils_get_int(packet, 0);
            //unsigned short payload_type = byteutils_get_short(packet, 4) & 0xff;
            //unsigned short payload_option = byteutils_get_short(packet, 6);

            if (payload == NULL) {
                payload = malloc(payload_size);
                readstart = 0;
            }

            while (readstart < payload_size) {
                // Payload data
                ret = recv(stream_fd, payload + readstart, payload_size - readstart, 0);
                if (ret <= 0) break;
                readstart = readstart + ret;
            }

            if (ret == 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror tcp socket closed");
                break;
            } else if (ret == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // Timeouts can happen even if the connection is fine
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror error in recv: %d %s", errno, strerror(errno));
                if (errno == ECONNRESET) conn_reset = true;
                break;
            }

	    switch (packet[4]) {
            case  0x00:
                // Normal video data (VCL NAL)

                // Conveniently, the video data is already stamped with the remote wall clock time,
                // so no additional clock syncing needed. The only thing odd here is that the video
                // ntp time stamps don't include the SECONDS_FROM_1900_TO_1970, so it's really just
                // counting micro seconds since last boot.
                ntp_timestamp_raw = byteutils_get_long(packet, 8);
                uint64_t ntp_timestamp_remote = raop_ntp_timestamp_to_micro_seconds(ntp_timestamp_raw, false);
                uint64_t ntp_timestamp = raop_ntp_convert_remote_time(raop_rtp_mirror->ntp, ntp_timestamp_remote);

                uint64_t ntp_now = raop_ntp_get_local_time(raop_rtp_mirror->ntp);
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror video ntp = %llu, now = %llu, latency = %lld",
                           ntp_timestamp, ntp_now, ((int64_t) ntp_now) - ((int64_t) ntp_timestamp));

#ifdef DUMP_H264
                fwrite(payload, payload_size, 1, file_source);
                fwrite(&readstart, sizeof(readstart), 1, file_len);
#endif
                unsigned char* payload_out;
		unsigned char* payload_decrypted;
                bool prepend_sps_pps = ((packet[5] != 0x00) && (raop_rtp_mirror->sps_pps_len > 0));
                if (prepend_sps_pps) {
                    payload_out = (unsigned char*)  malloc(payload_size + raop_rtp_mirror->sps_pps_len);
                    payload_decrypted = payload_out + raop_rtp_mirror->sps_pps_len;
                    memcpy(payload_out, raop_rtp_mirror->sps_pps, raop_rtp_mirror->sps_pps_len);
                } else {
                    payload_out = (unsigned char*)  malloc(payload_size);
                    payload_decrypted = payload_out;
                }
                // Decrypt data
                mirror_buffer_decrypt(raop_rtp_mirror->buffer, payload, payload_decrypted, payload_size);

                // It seems the AirPlay protocol prepends NALs with their size, which we're replacing with the 4-byte
                // start code for the NAL Byte-Stream Format.
                bool valid_data = true;
                int nalu_size = 0;
                int nalus_count = 0;
                while (nalu_size < payload_size) {
                    int nc_len = byteutils_get_int_be(payload_decrypted, nalu_size);
                    if (nc_len < 0 || nalu_size + 4 > payload_size) {
                        valid_data = false;
                        break;
                    }
                    payload_decrypted[nalu_size + 0] = 0;
                    payload_decrypted[nalu_size + 1] = 0;
                    payload_decrypted[nalu_size + 2] = 0;
                    payload_decrypted[nalu_size + 3] = 1;
                    nalu_size += 4;
                    nalus_count++;
                    nalu_size += nc_len;
                }
                if (nalu_size != payload_size) valid_data = false;

                // int nalu_type = payload[4] & 0x1f;
                // logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "nalutype = %d", nalu_type);
                // logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "nalu_size = %d, payloadsize = %d nalus_count = %d",
                //        nalu_size, payload_size, nalus_count);

#ifdef DUMP_H264
                fwrite(payload_decrypted, payload_size, 1, file);
#endif

		h264_decode_struct h264_data;
                h264_data.pts = ntp_timestamp;
                h264_data.frame_type = payload_decrypted[4] & 0x1f;
                if (h264_data.frame_type == 5) {
		    assert(prepend_sps_pps);
                    h264_data.data = payload_out;
                    h264_data.data_len = payload_size + raop_rtp_mirror->sps_pps_len;
                    if (ntp_timestamp_raw != ntp_timestamp_nal) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_WARNING, "raop_rtp_mirror: prepended sps_pps timestamp does not match that of video payload");
                    }
                } else {
                    h264_data.data_len = payload_size;
                    h264_data.data = payload_decrypted;
                }


		if (!valid_data) h264_data.data[0] = 1; /* mark video data as invalid h264 (failed decryption) */
                raop_rtp_mirror->callbacks.video_process(raop_rtp_mirror->callbacks.cls, raop_rtp_mirror->ntp, &h264_data);
                free(payload_out);
                break;
            case 0x01:
                // The information in the payload contains an SPS and a PPS NAL
                ntp_timestamp_nal = byteutils_get_long(packet, 8);
                float width_source = byteutils_get_float(packet, 40);
                float height_source = byteutils_get_float(packet, 44);
                float width = byteutils_get_float(packet, 56);
                float height = byteutils_get_float(packet, 60);
                if (raop_rtp_mirror->callbacks.video_report_size) {
                    raop_rtp_mirror->callbacks.video_report_size(raop_rtp_mirror->callbacks.cls, &width_source, &height_source, &width, &height);
                }
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror width_source = %f height_source = %f width = %f height = %f",
                           width_source, height_source, width, height);

                // The sps_pps is not encrypted
                h264codec_t h264;
                h264.version = payload[0];
                h264.profile_high = payload[1];
                h264.compatibility = payload[2];
                h264.level = payload[3];
                h264.reserved_6_and_nal = payload[4];
                h264.reserved_3_and_sps = payload[5];
                h264.sps_size = (short) (((payload[6] & 255) << 8) + (payload[7] & 255));
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror sps size = %d", h264.sps_size);
                h264.sequence_parameter_set = malloc(h264.sps_size);
                memcpy(h264.sequence_parameter_set, payload + 8, h264.sps_size);
                h264.number_of_pps = payload[h264.sps_size + 8];
                h264.pps_size = (short) (((payload[h264.sps_size + 9] & 2040) + payload[h264.sps_size + 10]) & 255);
                h264.picture_parameter_set = malloc(h264.pps_size);
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror pps size = %d", h264.pps_size);
                memcpy(h264.picture_parameter_set, payload + h264.sps_size + 11, h264.pps_size);

                if (h264.sps_size + h264.pps_size < 102400) {
                    // Copy the sps and pps into a buffer to hand to the decoder
                    raop_rtp_mirror->sps_pps_len = (h264.sps_size + h264.pps_size) + 8;
		    if (raop_rtp_mirror->sps_pps) {
                        free(raop_rtp_mirror->sps_pps);
                        raop_rtp_mirror->sps_pps = NULL;
		    }
                    raop_rtp_mirror->sps_pps = (unsigned char*) malloc(raop_rtp_mirror->sps_pps_len);
                    assert(raop_rtp_mirror->sps_pps);
                    raop_rtp_mirror->sps_pps[0] = 0;
                    raop_rtp_mirror->sps_pps[1] = 0;
                    raop_rtp_mirror->sps_pps[2] = 0;
                    raop_rtp_mirror->sps_pps[3] = 1;
                    memcpy(raop_rtp_mirror->sps_pps + 4, h264.sequence_parameter_set, h264.sps_size);
                    raop_rtp_mirror->sps_pps[h264.sps_size + 4] = 0;
                    raop_rtp_mirror->sps_pps[h264.sps_size + 5] = 0;
                    raop_rtp_mirror->sps_pps[h264.sps_size + 6] = 0;
                    raop_rtp_mirror->sps_pps[h264.sps_size + 7] = 1;
                    memcpy(raop_rtp_mirror->sps_pps + h264.sps_size + 8, h264.picture_parameter_set, h264.pps_size);

#ifdef DUMP_H264
                    fwrite(raop_rtp_mirror->sps_pps, raop_rtp_mirror->sps_pps_len, 1, file);
#endif

                }
                free(h264.picture_parameter_set);
                free(h264.sequence_parameter_set);
                break;
            case 0x05:
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "\nReceived video streaming performance info packet from client");
	      /* payloads with packet[4] = 0x05 have no timestamp, and carry video info from the client as a binary plist *
               * Sometimes (e.g, when the client has a locked screen), there is a 25kB trailer attached to the packet.    *
	       * This 25000 Byte trailer with unidentified content seems to be the same data each time it is sent.        */

                if (payload_size && raop_rtp_mirror->show_client_FPS_data) {
                    //char *str = utils_data_to_string(packet, 128, 16);
                    //logger_log(raop_rtp_mirror->logger, LOGGER_WARNING, "type 5 video packet header:\n%s", str);
                    //free (str);
		    
                    int plist_size = payload_size;
                    if (payload_size > 25000) {
		        plist_size = payload_size - 25000;
                        char *str = utils_data_to_string(payload + plist_size, 16, 16);
                        logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "video_info packet had 25kB trailer; first 16 bytes are:\n%s", str);
                        free(str);
                    }
                    if (plist_size) {
                        char *plist_xml;
                        uint32_t plist_len;
                        plist_t root_node = NULL;
                        plist_from_bin((char *) payload, plist_size, &root_node);
                        plist_to_xml(root_node, &plist_xml, &plist_len);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "%s", plist_xml);
                        free(plist_xml);
                    }
                }
            default:
                break;
            }

            free(payload);
            payload = NULL;
            memset(packet, 0, 128);
            readstart = 0;
        }
    }

    /* Close the stream file descriptor */
    if (stream_fd != -1) {
        closesocket(stream_fd);
    }

#ifdef DUMP_H264
    fclose(file);
    fclose(file_source);
    fclose(file_len);
#endif

    // Ensure running reflects the actual state
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    raop_rtp_mirror->running = false;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror exiting TCP thread");
    if (conn_reset && raop_rtp_mirror->callbacks.conn_reset) {
      raop_rtp_mirror->callbacks.conn_reset(raop_rtp_mirror->callbacks.cls, 0);
    }
    return 0;
}

static int
raop_rtp_init_mirror_sockets(raop_rtp_mirror_t *raop_rtp_mirror, int use_ipv6);

void
raop_rtp_start_mirror(raop_rtp_mirror_t *raop_rtp_mirror, int use_udp, unsigned short *mirror_data_lport, uint8_t show_client_FPS_data)
{
    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "raop_rtp_mirror starting mirroring");
    int use_ipv6 = 0;

    assert(raop_rtp_mirror);
    assert(mirror_data_lport);
    raop_rtp_mirror->show_client_FPS_data = show_client_FPS_data;

    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (raop_rtp_mirror->running || !raop_rtp_mirror->joined) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }

    if (raop_rtp_mirror->remote_saddr.ss_family == AF_INET6) {
        use_ipv6 = 1;
    }
    use_ipv6 = 0;
     
    raop_rtp_mirror->mirror_data_lport = *mirror_data_lport;
    if (raop_rtp_init_mirror_sockets(raop_rtp_mirror, use_ipv6) < 0) {
        logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror initializing sockets failed");
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    *mirror_data_lport = raop_rtp_mirror->mirror_data_lport;

    /* Create the thread and initialize running values */
    raop_rtp_mirror->running = 1;
    raop_rtp_mirror->joined = 0;

    THREAD_CREATE(raop_rtp_mirror->thread_mirror, raop_rtp_mirror_thread, raop_rtp_mirror);
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
}

void raop_rtp_mirror_stop(raop_rtp_mirror_t *raop_rtp_mirror) {
    assert(raop_rtp_mirror);

    /* Check that we are running and thread is not
     * joined (should never be while still running) */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (!raop_rtp_mirror->running || raop_rtp_mirror->joined) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    raop_rtp_mirror->running = 0;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

    if (raop_rtp_mirror->mirror_data_sock != -1) {
        closesocket(raop_rtp_mirror->mirror_data_sock);
        raop_rtp_mirror->mirror_data_sock = -1;
    }

    /* Join the thread */
    THREAD_JOIN(raop_rtp_mirror->thread_mirror);

    /* Mark thread as joined */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    raop_rtp_mirror->joined = 1;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
}

void raop_rtp_mirror_destroy(raop_rtp_mirror_t *raop_rtp_mirror) {
    if (raop_rtp_mirror) {
        raop_rtp_mirror_stop(raop_rtp_mirror);
        MUTEX_DESTROY(raop_rtp_mirror->run_mutex);
        mirror_buffer_destroy(raop_rtp_mirror->buffer);
        if (raop_rtp_mirror->sps_pps) {
            free(raop_rtp_mirror->sps_pps);
        }
	free(raop_rtp_mirror);
    }
}

static int
raop_rtp_init_mirror_sockets(raop_rtp_mirror_t *raop_rtp_mirror, int use_ipv6)
{
    assert(raop_rtp_mirror);

    unsigned short dport = raop_rtp_mirror->mirror_data_lport;
    int dsock = netutils_init_socket(&dport, use_ipv6, 0);
    if (dsock == -1) {
        goto sockets_cleanup;
    }

    /* Listen to the data socket if using TCP */
    if (listen(dsock, 1) < 0) {
        goto sockets_cleanup;
    }

    /* Set socket descriptors */
    raop_rtp_mirror->mirror_data_sock = dsock;

    /* Set port values */
    raop_rtp_mirror->mirror_data_lport = dport;
    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror local data port socket %d port TCP %d", dsock, dport);
    return 0;

    sockets_cleanup:
    if (dsock != -1) closesocket(dsock);
    return -1;
}
