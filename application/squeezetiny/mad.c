/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "squeezelite.h"

#include <mad.h>

#define MAD_DELAY 529

#define READBUF_SIZE 2048 // local buffer used by decoder: FIXME merge with any other decoders needing one?

#if !LINKALL
static struct {
	// mad symbols to be dynamically loaded
	void (* mad_stream_init)(struct mad_stream *);
	void (* mad_frame_init)(struct mad_frame *);
	void (* mad_synth_init)(struct mad_synth *);
	void (* mad_frame_finish)(struct mad_frame *);
	void (* mad_stream_finish)(struct mad_stream *);
	void (* mad_stream_buffer)(struct mad_stream *, unsigned char const *, unsigned long);
	int  (* mad_frame_decode)(struct mad_frame *, struct mad_stream *);
	void (* mad_synth_frame)(struct mad_synth *, struct mad_frame const *);
	char const *(* mad_stream_errorstr)(struct mad_stream const *);
} gm;
#endif

struct mad {
	u8_t *readbuf;
	unsigned readbuf_len;
	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;
	enum mad_error last_error;
	// for lame gapless processing
	int checktags;
	u32_t consume;
	u32_t skip;
	u64_t samples;
	u32_t padding;
};


extern log_level decode_loglevel;
static log_level *loglevel = &decode_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#if PROCESS
#define LOCK_O_direct   if (ctx->decode.direct) mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct if (ctx->decode.direct) mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    if (ctx->decode.direct) { x }
#define IF_PROCESS(x)   if (!ctx->decode.direct) { x }
#else
#define LOCK_O_direct   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#endif

#if LINKALL
#define MAD(h, fn, ...) (mad_ ## fn)(__VA_ARGS__)
#else
#define MAD(h, fn, ...) (h)->mad_##fn(__VA_ARGS__)
#endif

// based on libmad minimad.c scale
static inline u16_t scale(mad_fixed_t sample) {
	sample += (1L << (MAD_F_FRACBITS - 24));

	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;

	return (s16_t)((sample >> (MAD_F_FRACBITS + 1 - 24)) >> 8);
}

// check for id3.2 tag at start of file - http://id3.org/id3v2.4.0-structure, return length
static unsigned _check_id3_tag(size_t bytes, struct thread_ctx_s *ctx) {
	u8_t *ptr = ctx->streambuf->readp;
	u32_t size = 0;

	if (bytes > 10 && *ptr == 'I' && *(ptr+1) == 'D' && *(ptr+2) == '3') {
		// size is encoded as syncsafe integer, add 10 if footer present
		if (*(ptr+6) < 0x80 && *(ptr+7) < 0x80 && *(ptr+8) < 0x80 && *(ptr+9) < 0x80) {
			size = 10 + (*(ptr+6) << 21) + (*(ptr+7) << 14) + (*(ptr+8) << 7) + *(ptr+9) + ((*(ptr+5) & 0x10) ? 10 : 0);
			LOG_DEBUG("id3.2 tag len: %u", size);
		}
	}

	return size;
}

// check for lame gapless params, don't advance streambuf
static void _check_lame_header(size_t bytes, struct thread_ctx_s *ctx) {
	u8_t *ptr = ctx->streambuf->readp;
	struct mad *m = ctx->decode.handle;

	if (*ptr == 0xff && (*(ptr+1) & 0xf0) == 0xf0 && bytes > 180) {

		u32_t frame_count = 0, enc_delay = 0, enc_padding = 0;
		u8_t flags;

		// 2 channels
		if (!memcmp(ptr + 36, "Xing", 4) || !memcmp(ptr + 36, "Info", 4)) {
			ptr += 36 + 7;
		// mono	
		} else if (!memcmp(ptr + 21, "Xing", 4) || !memcmp(ptr + 21, "Info", 4)) {
			ptr += 21 + 7;
		}

		flags = *ptr;

		if (flags & 0x01) {
			frame_count = unpackN((u32_t *)(ptr + 1));
			ptr += 4;
		}
		if (flags & 0x02) ptr += 4;
		if (flags & 0x04) ptr += 100;
		if (flags & 0x08) ptr += 4;

		if (!!memcmp(ptr+1, "LAME", 4)) {
			return;
		}

		ptr += 22;

		enc_delay   = (*ptr << 4 | *(ptr + 1) >> 4) + MAD_DELAY;
		enc_padding = (*(ptr + 1) & 0xF) << 8 | *(ptr + 2);
		enc_padding = enc_padding > MAD_DELAY ? enc_padding - MAD_DELAY : 0;

		// add one frame to initial skip for this (empty) frame
		m->skip    = enc_delay + 1152;
		m->samples = frame_count * 1152 - enc_delay - enc_padding;
		m->padding = enc_padding;
		
		LOG_INFO("gapless: skip: %u samples: " FMT_u64 " delay: %u padding: %u", m->skip, m->samples, enc_delay, enc_padding);
	}
}

static decode_state mad_decode(struct thread_ctx_s *ctx) {
	size_t bytes;
	bool eos = false;
	struct mad *m = ctx->decode.handle;

	LOCK_S;
	bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	if (m->checktags) {
		if (m->checktags == 1) {
			m->consume = _check_id3_tag(bytes, ctx);
			m->checktags = 2;
		}
		if (m->consume) {
			u32_t consume = min(m->consume, bytes);
			LOG_DEBUG("consume: %u of %u", consume, m->consume);
			_buf_inc_readp(ctx->streambuf, consume);
			m->consume -= consume;
			UNLOCK_S;
			return DECODE_RUNNING;
		}
		if (m->checktags == 2) {
			if (!ctx->stream.meta_interval) {
				_check_lame_header(bytes, ctx);
			}
			m->checktags = 0;
		}
	}

	if (m->stream.next_frame && m->readbuf_len) {
		m->readbuf_len -= m->stream.next_frame - m->readbuf;
		memmove(m->readbuf, m->stream.next_frame, m->readbuf_len);
	}

	bytes = min(bytes, READBUF_SIZE - m->readbuf_len);
	memcpy(m->readbuf + m->readbuf_len, ctx->streambuf->readp, bytes);
	m->readbuf_len += bytes;
	_buf_inc_readp(ctx->streambuf, bytes);

	if (ctx->stream.state <= DISCONNECT && _buf_used(ctx->streambuf) == 0) {
		eos = true;
		LOG_DEBUG("[%p]: end of stream", ctx);
		memset(m->readbuf + m->readbuf_len, 0, MAD_BUFFER_GUARD);
		m->readbuf_len += MAD_BUFFER_GUARD;
	}

	UNLOCK_S;

	MAD(&gm, stream_buffer, &m->stream, m->readbuf, m->readbuf_len);

	while (true) {
		size_t frames;
		s32_t *iptrl;
		s32_t *iptrr;
		unsigned max_frames;

		if (MAD(&gm, frame_decode, &m->frame, &m->stream) == -1) {
			decode_state ret;
			if (!eos && m->stream.error == MAD_ERROR_BUFLEN) {
				ret = DECODE_RUNNING;
			} else if (eos && (m->stream.error == MAD_ERROR_BUFLEN || m->stream.error == MAD_ERROR_LOSTSYNC)) {
				ret = DECODE_COMPLETE;
			} else if (!MAD_RECOVERABLE(m->stream.error)) {
				LOG_INFO("[%p]: mad_frame_decode error: %s - stopping decoder", ctx, MAD(&gm, stream_errorstr, &m->stream));
				ret = DECODE_COMPLETE;
			} else {
				if (m->stream.error != m->last_error) {
					// suppress repeat error messages
					LOG_DEBUG("[%p]: mad_frame_decode error: %s", ctx, MAD(&gm, stream_errorstr, &m->stream));
				}
				ret = DECODE_RUNNING;
			}
			m->last_error = m->stream.error;
			return ret;
		};

		MAD(&gm, synth_frame, &m->synth, &m->frame);

		if (ctx->decode.new_stream) {
			LOCK_O;
			LOG_INFO("[%p]: setting track_start", ctx);
			//output.next_sample_rate = decode_newstream(m->synth.pcm.samplerate, ctx->output.supported_rates);
			ctx->output.current_sample_rate = decode_newstream(m->synth.pcm.samplerate, ctx->output.supported_rates, ctx);
			ctx->output.track_start = ctx->outputbuf->writep;
			if (ctx->output.fade_mode) _checkfade(true, ctx);
			ctx->decode.new_stream = false;
			UNLOCK_O;
		}

		LOCK_O_direct;

		IF_DIRECT(
			max_frames = _buf_space(ctx->outputbuf) / BYTES_PER_FRAME;
		);
		IF_PROCESS(
			max_frames = ctx->process.max_in_frames - ctx->process.in_frames;
		);

		if (m->synth.pcm.length > max_frames) {
			LOG_WARN("[%p]: too many samples - dropping samples", ctx);
			m->synth.pcm.length = max_frames;
		}

		frames = m->synth.pcm.length;
		iptrl = m->synth.pcm.samples[0];
		iptrr = m->synth.pcm.samples[ m->synth.pcm.channels - 1 ];

		if (m->skip) {
			u32_t skip = min(m->skip, frames);
			LOG_DEBUG("[%p]: gapless: skipping %u frames at start", ctx, skip);
			frames -= skip;
			m->skip -= skip;
			iptrl += skip;
			iptrr += skip;
		}

		if (m->samples) {
			if (m->samples < frames) {
				LOG_DEBUG("[%p]: gapless: trimming %u frames from end", ctx, frames - m->samples);
				frames = (size_t)m->samples;
			}
			m->samples -= frames;
			if (m->samples > 0 && eos && !(m->stream.next_frame[0] == 0xff && (m->stream.next_frame[1] & 0xf0) == 0xf0)) {
				// this is the last frame to be decoded, but more samples expected so we must have skipped, remove padding
				// note this only works if the padding is less than one frame of 1152 bytes otherswise some gap will remain
				LOG_DEBUG("[%p]: gapless: early end - trimming padding from end", ctx);
				if (frames >= m->padding) {
					frames -= m->padding;
				} else {
					frames = 0;
				}
				m->samples = 0;
			}
		}

		LOG_SDEBUG("[%p]: write %u frames", ctx, frames);

		while (frames > 0) {
			size_t f, count;
			s16_t *optr;

			IF_DIRECT(
				f = min(frames, _buf_cont_write(ctx->outputbuf) / BYTES_PER_FRAME);
				optr = (s16_t *)ctx->outputbuf->writep;
			);
			IF_PROCESS(
				f = min(frames, ctx->process.max_in_frames - ctx->process.in_frames);
				optr = (s16_t *)((u8_t *) ctx->process.inbuf + ctx->process.in_frames * BYTES_PER_FRAME);
			);

			count = f;

			while (count--) {
				*optr++ = scale(*iptrl++);
				*optr++ = scale(*iptrr++);
			}

			frames -= f;

			IF_DIRECT(
				_buf_inc_writep(ctx->outputbuf, f * BYTES_PER_FRAME);
			);
			IF_PROCESS(
				ctx->process.in_frames += f;
			);
		}

		UNLOCK_O_direct;
	}

	return eos ? DECODE_COMPLETE : DECODE_RUNNING;
}

static void mad_open(u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx) {
	struct mad *m = ctx->decode.handle;

	if (!m) {
		m = ctx->decode.handle = malloc(sizeof(struct mad));
		if (!m) return;
		m->readbuf = NULL;
	}

	if (!m->readbuf) m->readbuf = malloc(READBUF_SIZE + MAD_BUFFER_GUARD);
	m->checktags = 1;
	m->consume = 0;
	m->skip = MAD_DELAY;
	m->samples = 0;
	m->readbuf_len = 0;
	m->last_error = MAD_ERROR_NONE;
	m->readbuf_len = 0;

	MAD(&gm, stream_init, &m->stream);
	MAD(&gm, frame_init, &m->frame);
	MAD(&gm, synth_init, &m->synth);
}

static void mad_close(struct thread_ctx_s *ctx) {
	struct mad *m = ctx->decode.handle;

	mad_synth_finish(&m->synth); // macro only in current version
	MAD(&gm, frame_finish, &m->frame);
	MAD(&gm, stream_finish, &m->stream);
	free(m->readbuf);
	free(m);
	ctx->decode.handle = NULL;
}

static bool load_mad(void) {
#if !LINKALL
	void *handle = dlopen(LIBMAD, RTLD_NOW);
	char *err;

	if (!handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	gm.mad_stream_init = dlsym(handle, "mad_stream_init");
	gm.mad_frame_init = dlsym(handle, "mad_frame_init");
	gm.mad_synth_init = dlsym(handle, "mad_synth_init");
	gm.mad_frame_finish = dlsym(handle, "mad_frame_finish");
	gm.mad_stream_finish = dlsym(handle, "mad_stream_finish");
	gm.mad_stream_buffer = dlsym(handle, "mad_stream_buffer");
	gm.mad_frame_decode = dlsym(handle, "mad_frame_decode");
	gm.mad_synth_frame = dlsym(handle, "mad_synth_frame");
	gm.mad_stream_errorstr = dlsym(handle, "mad_stream_errorstr");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded "LIBMAD, NULL);
#endif

	return true;
}

struct codec *register_mad(void) {
	static struct codec ret = {
		'm',          // id
		"mp3",        // types
		READBUF_SIZE, // min read
		206800,       // min space
		mad_open,     // open
		mad_close,    // close
		mad_decode,   // decode
	};

	if (!load_mad()) {
		return NULL;
	}

	LOG_INFO("using mad to decode mp3", NULL);
	return &ret;
}
