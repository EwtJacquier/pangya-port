// sceAtrac3plus: music and voice streams (ATRAC3 / ATRAC3+ in RIFF WAVE), decoded with FFmpeg.
//
// The PSP API streams the file through a guest buffer that the game refills. Instead of
// reproducing that ring-buffer protocol, the runtime finds which file the buffer was read from
// (io_find_source), loads the whole RIFF from the disc and tells the game that everything is
// already in memory (remainFrame = -1), so it never needs to stream.
#include <libavcodec/avcodec.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "rt.h"

#define ERR_NO_ID 0x80630003u
#define ERR_ALL_DECODED 0x80630024u
#define ERR_BAD_ID 0x80630005u

typedef struct {
	bool used;
	uint8_t *file;      // whole RIFF
	uint32_t file_size;
	uint32_t data_off, data_size, block_align;
	uint32_t fmt_off, fmt_size;
	int channels, codec;  // codec: 0x270 ATRAC3, 0xFFFE ATRAC3+
	int frame_samples;
	uint32_t total_frames, cur_frame;
	int loop_start_frame, loop_end_frame;  // -1 = no loop info
	int loop_num;                          // -1 infinite
	uint32_t guest_buf;
	AVCodecContext *ctx;
	AVFrame *frame;
} Atrac;

static Atrac atracs[8];

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool parse_riff(Atrac *a) {
	const uint8_t *f = a->file;
	if (a->file_size < 12 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4)) return false;
	uint32_t pos = 12;
	a->loop_start_frame = a->loop_end_frame = -1;
	while (pos + 8 <= a->file_size) {
		uint32_t sz = rd32(f + pos + 4);
		const uint8_t *body = f + pos + 8;
		if (!memcmp(f + pos, "fmt ", 4)) {
			a->fmt_off = pos + 8;
			a->fmt_size = sz;
			a->codec = rd16(body);
			a->channels = rd16(body + 2);
			a->block_align = rd16(body + 12);
		}
		else if (!memcmp(f + pos, "smpl", 4) && sz >= 36 + 24 && rd32(body + 28) > 0) {
			uint32_t start = rd32(body + 36 + 8), end = rd32(body + 36 + 12);
			a->loop_start_frame = (int)(start / 2048);
			a->loop_end_frame = (int)(end / 2048);
		}
		else if (!memcmp(f + pos, "data", 4)) {
			a->data_off = pos + 8;
			a->data_size = sz;
			if (a->data_off + a->data_size > a->file_size) a->data_size = a->file_size - a->data_off;
			break;
		}
		pos += 8 + sz + (sz & 1);
	}
	if (!a->data_off || !a->block_align) return false;
	a->frame_samples = a->codec == 0x270 ? 1024 : 2048;
	if (a->codec == 0x270) {
		a->loop_start_frame = a->loop_start_frame < 0 ? -1 : a->loop_start_frame * 2;
		a->loop_end_frame = a->loop_end_frame < 0 ? -1 : a->loop_end_frame * 2;
	}
	a->total_frames = a->data_size / a->block_align;
	return true;
}

static bool open_decoder(Atrac *a) {
	static bool registered;
	if (!registered) {
		avcodec_register_all();
		av_log_set_level(AV_LOG_QUIET);
		registered = true;
	}
	AVCodec *codec = avcodec_find_decoder(a->codec == 0x270 ? AV_CODEC_ID_ATRAC3 : AV_CODEC_ID_ATRAC3P);
	if (!codec) return false;
	a->ctx = avcodec_alloc_context3(codec);
	a->ctx->channels = a->channels;
	a->ctx->channel_layout = a->channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
	a->ctx->sample_rate = 44100;
	a->ctx->block_align = (int)a->block_align;
	if (a->codec == 0x270) {  // ATRAC3 needs the 14-byte codec extradata that follows cbSize in fmt
		static const uint8_t fallback[14] = {1, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
		const uint8_t *extra = a->fmt_size >= 18 + 14 ? a->file + a->fmt_off + 18 : fallback;
		a->ctx->extradata = av_mallocz(14 + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(a->ctx->extradata, extra, 14);
		a->ctx->extradata_size = 14;
	}
	if (avcodec_open2(a->ctx, codec, NULL) < 0) return false;
	a->frame = av_frame_alloc();
	return true;
}

static void atrac_free(Atrac *a) {
	if (a->ctx) avcodec_free_context(&a->ctx);
	if (a->frame) av_frame_free(&a->frame);
	free(a->file);
	memset(a, 0, sizeof *a);
}

void hle_sceAtracSetDataAndGetID(CPU *c) {
	uint32_t buf = A0, bufsize = A1;
	int id = -1;
	for (int i = 0; i < 8; i++) {
		if (!atracs[i].used) {
			id = i;
			break;
		}
	}
	if (id < 0) {
		RET(ERR_NO_ID);
		return;
	}
	Atrac *a = &atracs[id];
	memset(a, 0, sizeof *a);
	if (!rt_valid(buf, 12) || memcmp(MP(buf), "RIFF", 4)) {
		rt_log("sceAtracSetDataAndGetID(%08x): not a RIFF buffer", buf);
		RET(0x80630011u);
		return;
	}
	uint32_t riff_size = rd32(MP(buf + 4)) + 8;
	a->file = calloc(1, (size_t)riff_size + AV_INPUT_BUFFER_PADDING_SIZE);  // FFmpeg reads past the last frame
	a->file_size = riff_size;
	uint64_t iso_off;
	if (riff_size <= bufsize) {
		memcpy(a->file, MP(buf), riff_size);
	}
	else if (io_find_source(buf, &iso_off)) {
		io_iso_read(iso_off, a->file, riff_size);
	}
	else {
		memcpy(a->file, MP(buf), bufsize);
		a->file_size = bufsize;
		rt_log("sceAtracSetDataAndGetID: source of partial buffer %08x unknown; playing what is loaded", buf);
	}
	a->guest_buf = buf;
	a->loop_num = 0;
	if (!parse_riff(a) || !open_decoder(a)) {
		rt_log("sceAtracSetDataAndGetID: unsupported stream (codec %x)", a->codec);
		atrac_free(a);
		RET(0x80630011u);
		return;
	}
	a->used = true;
	TRACE("sceAtracSetDataAndGetID(%08x, %x) -> %d: codec %x ch %d frames %u loop %d-%d", buf, bufsize, id, a->codec,
	      a->channels, a->total_frames, a->loop_start_frame, a->loop_end_frame);
	RET(id);
}

static Atrac *get(uint32_t id) { return id < 8 && atracs[id].used ? &atracs[id] : NULL; }

void hle_sceAtracReleaseAtracID(CPU *c) {
	Atrac *a = get(A0);
	if (a) atrac_free(a);
	RET(a ? 0 : ERR_BAD_ID);
}

void hle_sceAtracDecodeData(CPU *c) {
	Atrac *a = get(A0);
	uint32_t out = A1, nsamp = A2, finish = A3, remain = A4;
	if (!a) {
		RET(ERR_BAD_ID);
		return;
	}
	if (a->loop_end_frame >= 0 && a->loop_num != 0 && (int)a->cur_frame > a->loop_end_frame) {
		a->cur_frame = (uint32_t)a->loop_start_frame;
		if (a->loop_num > 0) a->loop_num--;
	}
	if (a->cur_frame >= a->total_frames) {
		if (nsamp) M32(nsamp) = 0;
		if (finish) M32(finish) = 1;
		if (remain) M32(remain) = 0xFFFFFFFFu;
		RET(ERR_ALL_DECODED);
		return;
	}
	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = a->file + a->data_off + a->cur_frame * a->block_align;
	pkt.size = (int)a->block_align;
	int got = 0;
	int samples = 0;
	if (avcodec_decode_audio4(a->ctx, a->frame, &got, &pkt) >= 0 && got) {
		samples = a->frame->nb_samples;
		int16_t *o = out ? (int16_t *)MP(out) : NULL;
		int ch = a->channels;
		for (int i = 0; o && i < samples; i++) {
			for (int k = 0; k < ch; k++) {
				float v = ((float *)a->frame->extended_data[k])[i] * 32767.f;
				o[i * ch + k] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
			}
		}
	}
	else if (out) {
		memset(MP(out), 0, (size_t)a->frame_samples * 2 * (size_t)a->channels);
		samples = a->frame_samples;
	}
	a->cur_frame++;
	bool more = a->cur_frame < a->total_frames || (a->loop_end_frame >= 0 && a->loop_num != 0);
	if (nsamp) M32(nsamp) = (uint32_t)samples;
	if (finish) M32(finish) = more ? 0 : 1;
	if (remain) M32(remain) = 0xFFFFFFFFu;  // everything is in memory
	RET(0);
}

void hle_sceAtracGetStreamDataInfo(CPU *c) {
	Atrac *a = get(A0);
	if (A1) M32(A1) = a ? a->guest_buf : 0;
	if (A2) M32(A2) = 0;  // nothing to stream
	if (A3) M32(A3) = a ? a->file_size : 0;
	RET(a ? 0 : ERR_BAD_ID);
}
void hle_sceAtracAddStreamData(CPU *c) { RET(0); }
void hle_sceAtracGetRemainFrame(CPU *c) {
	if (A1) M32(A1) = 0xFFFFFFFFu;
	RET(get(A0) ? 0 : ERR_BAD_ID);
}
void hle_sceAtracSetLoopNum(CPU *c) {
	Atrac *a = get(A0);
	if (a) a->loop_num = (int)A1;
	RET(a ? 0 : ERR_BAD_ID);
}
void hle_sceAtracGetLoopStatus(CPU *c) {
	Atrac *a = get(A0);
	if (A1) M32(A1) = a ? (uint32_t)a->loop_num : 0;
	if (A2) M32(A2) = a && a->loop_end_frame >= 0 ? 1 : 0;
	RET(a ? 0 : ERR_BAD_ID);
}
void hle_sceAtracGetInternalErrorInfo(CPU *c) {
	if (A1) M32(A1) = 0;
	RET(0);
}
