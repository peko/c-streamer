//
// Created by user on 2/20/17.
//

#include <string.h>
#include <stdlib.h>
#include "amf/amf.h"
#include "rtmp/rtmp.h"

static size_t video_rmtp_writebasic(uint8_t type, uint32_t chunkid, void *data) {
    char *ptr = data;
    *ptr++ = (char) (chunkid & 0xFF) | (type << 6);
    if (chunkid >= 319) {
        *ptr++ = (char) (((chunkid - 64) >> 8) & 0xFF);
        *ptr++ = (char) ((chunkid - 64) & 0xFF);
    } else if (chunkid >= 64) {
        *ptr++ = (char) ((chunkid - 64) & 0xFF);
    }

    return ptr - (char*) data;
}

static void video_rtmp_writefree(uv_write_t *req, int status) {
    video_slab_unref(req->data);
    free(req);
}

static void video_rtmp_send(video_rtmp_local_t *local, uint32_t chunkid, uint32_t streamid, video_rtmp_msgtype type, uint32_t time, void *data, size_t len) {
    size_t totalsize = 11 + len + len / local->outchunksiz;
    if (chunkid >= 319) {
        totalsize += 3;
    } else if (chunkid >= 64) {
        totalsize += 2;
    } else {
        totalsize += 1;
    }

    if (time >= 0xFFFFFF) {
        totalsize += 4;
    }

    uv_buf_t buf = uv_buf_init(video_rtmp_alloc(local, totalsize), (unsigned int) totalsize);

    char *src = data;
    char *ptr = buf.base;
    ptr += video_rmtp_writebasic(0, chunkid, ptr);

    uint32_t timep = time > 0xFFFFFF ? 0xFFFFFF : time;
    memmove(ptr, (char*)&timep, 3);
    ptr += 3;

    uint32_t lenp = video_byte_swap_int32(len);
    memmove(ptr, (char*)&lenp+1, 3);
    ptr += 3;

    *ptr++ = type;

    memmove(ptr, &streamid, 4);
    ptr += 4;

    if (timep == 0xFFFFFF) {
        memmove(ptr, &time, 4);
        ptr += 4;
    }

    for (size_t i = 0; i < len / local->outchunksiz; i++) {
        if (i > 0) {
            ptr += video_rmtp_writebasic(3, chunkid, ptr);
        }
        memmove(ptr, src, local->outchunksiz);
        ptr += local->outchunksiz;
        src += local->outchunksiz;
    }

    size_t rem = len % local->outchunksiz;
    if (rem != 0) {
        if (len > local->outchunksiz) {
            ptr += video_rmtp_writebasic(3, chunkid, ptr);
        }
        memmove(ptr, src, rem);
        ptr += rem;
        src += rem;
    }

    uv_write_t *req = calloc(1, sizeof(uv_write_t));
    req->data = buf.base;
    uv_write(req, (uv_stream_t *) &local->client, &buf, 1, video_rtmp_writefree);
}

static void video_rtmp_command_connect(video_rtmp_local_t *local, video_list_t *list) {
    uint32_t ack_body = video_byte_swap_int32(5000000);
    video_rtmp_send(local, 2, 0, VIDEO_RTMP_MSGTYPE_WINDOW_ACKNOWLEDGEMENT_SIZE, 0, &ack_body, 4);

    char band_body[5];
    uint32_t band_value = video_byte_swap_int32(5000000);
    memmove(band_body, &band_value, 4);
    band_body[4] = 0x02;
    video_rtmp_send(local, 2, 0, VIDEO_RTMP_MSGTYPE_SET_PEER_BANDWIDTH, 0, &band_body, 5);

    uint32_t setchunk_body = video_byte_swap_int32(2048);
    video_rtmp_send(local, 2, 0, VIDEO_RTMP_MSGTYPE_SET_CHUNK_SIZE, 0, &setchunk_body, 4);
    local->outchunksiz = 2048;

    video_amf_value_t *res_str = video_amf_make_string("_result");
    video_amf_value_t *res_echo = list->nodes[1].data;
    video_amf_value_t *res_caps = video_amf_make_object();
    video_map_put(&res_caps->as.map, "fmsVer", video_amf_make_string("FMS/3,0,1,123"), video_amf_value_free);
    video_map_put(&res_caps->as.map, "capabilities", video_amf_make_number(31.0), video_amf_value_free);
    video_amf_value_t *res_stat = video_amf_make_object();
    video_map_put(&res_stat->as.map, "level", video_amf_make_string("status"), video_amf_value_free);
    video_map_put(&res_stat->as.map, "code", video_amf_make_string("NetConnection.Connect.Success"), video_amf_value_free);
    video_map_put(&res_stat->as.map, "description", video_amf_make_string("Connection succeeded."), video_amf_value_free);
    video_map_put(&res_stat->as.map, "objectEncoding", video_amf_make_number(3.0), video_amf_value_free);

    size_t esteem = video_amf_estimate(res_str)
                    + video_amf_estimate(res_echo)
                    + video_amf_estimate(res_caps)
                    + video_amf_estimate(res_stat);

    void *connect_body = video_rtmp_alloc(local, esteem);
    char *ptr = connect_body;
    ptr += video_amf_encode(res_str, ptr);
    ptr += video_amf_encode(res_echo, ptr);
    ptr += video_amf_encode(res_caps, ptr);
    ptr += video_amf_encode(res_stat, ptr);

    video_rtmp_send(local, 3, 0, VIDEO_RTMP_MSGTYPE_AMF0_CMD, 0, connect_body, esteem);
    video_slab_unref(connect_body);

    video_amf_value_free(res_str);
    video_amf_value_free(res_caps);
    video_amf_value_free(res_stat);
}

static void video_rtmp_command_create(video_rtmp_local_t *local, video_list_t *list) {
    video_amf_value_t *res_str = video_amf_make_string("_result");
    video_amf_value_t *res_echo = list->nodes[1].data;
    video_amf_value_t *res_null = video_amf_make_null();
    video_amf_value_t *res_num = video_amf_make_number(1.0);

    size_t esteem = video_amf_estimate(res_str)
                    + video_amf_estimate(res_echo)
                    + video_amf_estimate(res_null)
                    + video_amf_estimate(res_num);

    void *connect_body = video_rtmp_alloc(local, esteem);
    char *ptr = connect_body;
    ptr += video_amf_encode(res_str, ptr);
    ptr += video_amf_encode(res_echo, ptr);
    ptr += video_amf_encode(res_null, ptr);
    ptr += video_amf_encode(res_num, ptr);

    video_rtmp_send(local, 3, 0, VIDEO_RTMP_MSGTYPE_AMF0_CMD, 0, connect_body, esteem);
    video_slab_unref(connect_body);

    video_amf_value_free(res_str);
    video_amf_value_free(res_null);
    video_amf_value_free(res_num);
}

static void video_rtmp_command_publish(video_rtmp_local_t *local, video_list_t *list) {

}

static void video_rtmp_command_play(video_rtmp_local_t *local, video_list_t *list) {

}

static void video_rtmp_message(video_rtmp_local_t *local) {
    printf("[%p] recv: %s\n", local, video_rtmp_msgtype_names[local->header->type]);
    video_list_t list;
    switch (local->header->type) {
        case VIDEO_RTMP_MSGTYPE_SET_CHUNK_SIZE:
            printf("[%p] setting chunksize to %ld\n", local, local->inchunksiz);
            memmove(&local->inchunksiz, local->header->msgbodybuf.base, 4);
            break;
        case VIDEO_RTMP_MSGTYPE_AMF0_META:
        case VIDEO_RTMP_MSGTYPE_AMF3_META:
            break;
        case VIDEO_RTMP_MSGTYPE_AMF3_CMD_ALT:
        case VIDEO_RTMP_MSGTYPE_AMF3_CMD:;
        case VIDEO_RTMP_MSGTYPE_AMF0_CMD:
            video_list_init(&list);
            video_amf_decodeall(&list, local->header->msgbodybuf.base, local->header->length);
            video_amf_value_t *value = list.nodes[0].data;
            printf("[%p] CMD rcvd: %s\n", local, value->as.str);
            if (strcmp("connect", value->as.str) == 0) {
                video_rtmp_command_connect(local, &list);
            } else if (strcmp("createStream", value->as.str) == 0) {
                video_rtmp_command_create(local, &list);
            } else if (strcmp("publish", value->as.str) == 0) {
                video_rtmp_command_publish(local, &list);
            } else if (strcmp("play", value->as.str) == 0) {
                video_rtmp_command_play(local, &list);
            }
            video_list_deinit(&list);
            break;
        default:
            break;
    }
}

static void video_rtmp_chunk_free(void *ptr) {
    video_rtmp_chunk_t *chunk = ptr;
    video_slab_unref(chunk->msgbodybuf.base);
    video_slab_unref(chunk);
}

char *video_rtmp_header(video_rtmp_local_t *local, char *data, size_t len) {
    char *ptr = data;
    char *tgt = local->msgheaderbuf.base + local->msgheaderbuf.len;

    if (local->msgheaderbuf.len == 0) {
        local->msgheaderbuf.base[0] = ptr[0];
        local->msgheaderbuf.len = 1;
        ptr += 1;
        tgt += 1;
        if (len == 1) {
            return ptr;
        }
    }
    int headertype = local->msgheaderbuf.base[0] >> 6;
    size_t headerlen = 1;

    switch (headertype) {
        case 0:
            headerlen += 11;
            break;
        case 1:
            headerlen += 7;
            break;
        case 2:
            headerlen += 3;
            break;
        default:
        case 3:
            break;
    }
    switch (local->msgheaderbuf.base[0] & 0x3f) {
        case 0:
            headerlen += 1;
            break;
        case 1:
            headerlen += 2;
            break;
        default:
            break;
    }

    if (local->msgheaderbuf.len < headerlen) {
        size_t remaining = headerlen - local->msgheaderbuf.len;
        size_t avail = len - (ptr - data);
        if (remaining > avail) {
            remaining = avail;
        }
        memmove(tgt, ptr, remaining);
        ptr += remaining;
        local->msgheaderbuf.len += remaining;
        if (avail == remaining) {
            return ptr;
        }
    }

    uint32_t chunkid = (uint32_t) (local->msgheaderbuf.base[0] & 0x3f);

    switch (chunkid) {
        case 0:
            chunkid = (uint32_t) (64 + local->msgheaderbuf.base[1]);
            tgt = &local->msgheaderbuf.base[2];
            break;
        case 1:
            chunkid = (uint32_t) (64 + local->msgheaderbuf.base[1] * 256 + local->msgheaderbuf.base[2]);
            tgt = &local->msgheaderbuf.base[3];
            break;
        default:
            tgt = &local->msgheaderbuf.base[1];
            break;
    }

    local->header = 0;

    for (size_t i = 0; i < local->chunks.size; i++) {
        video_rtmp_chunk_t *c = local->chunks.nodes[i].data;
        if (c->chunkid == chunkid) {
            local->header = c;
            break;
        }
    }

    if (!local->header) {
        local->header = video_rtmp_alloc(local, sizeof(video_rtmp_chunk_t));
        local->header->chunkid = chunkid;
        local->header->msgbodybuf.base = video_rtmp_alloc(local, VIDEO_RTMP_MESSAGE_MAX);
        local->header->msgbodybuf.len = 0;
        video_list_add(&local->chunks, local->header, video_rtmp_chunk_free);
    }

    uint32_t time = 0;
    uint8_t  type = 0;
    switch (headertype) {
        case 0:
            local->header->streamid = 0;
            memmove(&local->header->streamid, tgt+7, 4);
            //local->header->streamid = video_byte_swap_32(local->header->streamid);
        case 1:
            local->header->type = VIDEO_RTMP_MSGTYPE_0;
            local->header->length = 0;
            memmove(&type, tgt+6, 1);
            local->header->type = (video_rtmp_msgtype) type;
            memmove((char*)&local->header->length + 1, tgt+3, 3);
            video_byte_swap_generic(&local->header->length, 4);
        case 2:
            memmove((char*)&time + 1, tgt, 3);
            if (headertype == 0) {
                local->header->timediff = 0;
                local->header->timestamp = time;
            } else {
                local->header->timediff = time;
            }
        default:
        case 3:
            local->header->timestamp += local->header->timediff;
            if (local->header->timestamp == 0xFFFFFF) {
                memmove(&local->header->timestamp, tgt, 4);
            }
    }

    if (local->header->length > VIDEO_RTMP_MESSAGE_MAX) {
        printf("[%p] Maximum message size exceeded, suggested: %d, max: %d\n", local, local->header->length, VIDEO_RTMP_MESSAGE_MAX);
        video_rtmp_disconnect(local);
    } else {
        local->state = VIDEO_RTMP_STATE_BODY;
    }

    return ptr;
}

char *video_rtmp_body(video_rtmp_local_t *local, char *ptr, size_t len) {
    size_t untilchunk = local->inchunksiz - (local->header->msgbodybuf.len % local->inchunksiz);
    size_t untildone = local->header->length - local->header->msgbodybuf.len;

    size_t remaining = (untilchunk > untildone) ? untildone : untilchunk;
    if (remaining > len) {
        remaining = len;
    }

    char *tgt = local->header->msgbodybuf.base + local->header->msgbodybuf.len;
    memmove(tgt, ptr, remaining);
    ptr += remaining;
    local->header->msgbodybuf.len += remaining;

    if (local->header->msgbodybuf.len == local->header->length) {
        video_rtmp_message(local);
        local->header->msgbodybuf.len = 0;
        local->msgheaderbuf.len = 0;
        local->state = VIDEO_RTMP_STATE_HEADER;
    } else if (local->header->msgbodybuf.len % local->inchunksiz == 0) {
        local->msgheaderbuf.len = 0;
        local->state = VIDEO_RTMP_STATE_HEADER;
    }

    return ptr;
}