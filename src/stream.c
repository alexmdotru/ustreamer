#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <sys/select.h>
#include <linux/videodev2.h>

#ifdef DUMP_STREAM_JPEGS
#	warning Enabled DUMP_STREAM_JPEGS
#	include <stdio.h>
#	include <fcntl.h>
#	include <sys/stat.h>
#	include <sys/types.h>
#endif

#include "tools.h"
#include "device.h"
#include "jpeg.h"
#include "stream.h"


static long double _stream_get_fluency_delay(struct device_t *dev, struct workers_pool_t *pool);
static int _stream_init_loop(struct device_t *dev, struct workers_pool_t *pool);
static int _stream_init(struct device_t *dev, struct workers_pool_t *pool);
static void _stream_init_workers(struct device_t *dev, struct workers_pool_t *pool);
static void *_stream_worker_thread(void *v_ctx);
static void _stream_destroy_workers(struct device_t *dev, struct workers_pool_t *pool);
static int _stream_control(struct device_t *dev, const bool enable);
static int _stream_grab_buffer(struct device_t *dev, struct v4l2_buffer *buf_info);
static int _stream_release_buffer(struct device_t *dev, struct v4l2_buffer *buf_info);
static int _stream_handle_event(struct device_t *dev);


struct stream_t *stream_init(struct device_t *dev) {
	struct stream_t *stream;

	A_CALLOC(stream, 1);
	stream->dev = dev;
	A_PTHREAD_M_INIT(&stream->mutex);
	return stream;
}

void stream_destroy(struct stream_t *stream) {
	A_PTHREAD_M_DESTROY(&stream->mutex);
	free(stream);
}

#ifdef DUMP_STREAM_JPEGS
static void _stream_dump(struct stream_t *stream) {
	static unsigned count = 0;
	char path[1024];

	errno = 0;
	mkdir("stream", 0777);
	assert(errno == 0 || errno == EEXIST);

	sprintf(path, "stream/img_%06u.jpg", count);
	int fd = open(path, O_CREAT|O_TRUNC|O_WRONLY, 0644);
	assert(fd);
	assert(write(fd, stream->picture.data, stream->picture.size) == (ssize_t)stream->picture.size);
	assert(!close(fd));

	LOG_INFO("-DDUMP_STREAM_JPEGS dumped %s", path);

	++count;
}
#endif

void stream_loop(struct stream_t *stream) {
	struct workers_pool_t pool;
	bool workers_stop;

	MEMSET_ZERO(pool);
	pool.workers_stop = &workers_stop;

	LOG_INFO("Using V4L2 device: %s", stream->dev->path);
	LOG_INFO("Using JPEG quality: %d%%", stream->dev->jpeg_quality);

	while (_stream_init_loop(stream->dev, &pool) == 0) {
		struct worker_t *last_worker = NULL;
		unsigned frames_count = 0;
		long double grab_after = 0;
		unsigned fluency_passed = 0;
		unsigned fps = 0;
		long long fps_second = 0;

		LOG_DEBUG("Allocation memory for stream picture ...");
		A_CALLOC(stream->picture.data, stream->dev->run->max_picture_size);

		A_PTHREAD_M_LOCK(&stream->mutex);
		stream->width = stream->dev->run->width;
		stream->height = stream->dev->run->height;
		stream->online = true;
		A_PTHREAD_M_UNLOCK(&stream->mutex);

		while (!stream->dev->stop) {
			SEP_DEBUG('-');

			LOG_DEBUG("Waiting for workers ...");
			A_PTHREAD_M_LOCK(&pool.has_free_workers_mutex);
			A_PTHREAD_C_WAIT_TRUE(pool.has_free_workers, &pool.has_free_workers_cond, &pool.has_free_workers_mutex);
			A_PTHREAD_M_UNLOCK(&pool.has_free_workers_mutex);

			if (last_worker && !last_worker->has_job && stream->dev->run->pictures[last_worker->ctx.index].data) {
				A_PTHREAD_M_LOCK(&stream->mutex);
				stream->picture.size = stream->dev->run->pictures[last_worker->ctx.index].size;
				stream->picture.allocated = stream->dev->run->pictures[last_worker->ctx.index].allocated;
				memcpy(
					stream->picture.data,
					stream->dev->run->pictures[last_worker->ctx.index].data,
					stream->picture.size * sizeof(*stream->picture.data)
				);
				stream->updated = true;
				A_PTHREAD_M_UNLOCK(&stream->mutex);

				last_worker = last_worker->order_next;

#	ifdef DUMP_STREAM_JPEGS
				_stream_dump(stream);
#	endif
			}

			if (stream->dev->stop) {
				break;
			}

#	define INIT_FD_SET(_set) \
		fd_set _set; FD_ZERO(&_set); FD_SET(stream->dev->run->fd, &_set);
			INIT_FD_SET(read_fds);
			INIT_FD_SET(write_fds);
			INIT_FD_SET(error_fds);
#	undef INIT_FD_SET

			struct timeval timeout;
			timeout.tv_sec = stream->dev->timeout;
			timeout.tv_usec = 0;

			LOG_DEBUG("Calling select() on video device ...");
			int retval = select(stream->dev->run->fd + 1, &read_fds, &write_fds, &error_fds, &timeout);
			LOG_DEBUG("Device select() --> %d", retval);

			if (retval < 0) {
				if (errno != EINTR) {
					LOG_PERROR("Mainloop select() error");
					break;
				}

			} else if (retval == 0) {
				LOG_ERROR("Mainloop select() timeout");
				break;

			} else {
				if (FD_ISSET(stream->dev->run->fd, &read_fds)) {
					LOG_DEBUG("Frame is ready");

					struct v4l2_buffer buf_info;

					if (_stream_grab_buffer(stream->dev, &buf_info) < 0) {
						break;
					}

					if (stream->dev->every_frame) {
						if (frames_count < stream->dev->every_frame - 1) {
							LOG_DEBUG("Dropping frame %d for option --every-frame=%d", frames_count + 1, stream->dev->every_frame);
							++frames_count;
							goto pass_frame;
						}
						frames_count = 0;
					}

					// Workaround for broken, corrupted frames:
					// Under low light conditions corrupted frames may get captured.
					// The good thing is such frames are quite small compared to the regular pictures.
					// For example a VGA (640x480) webcam picture is normally >= 8kByte large,
					// corrupted frames are smaller.
					if (buf_info.bytesused < stream->dev->min_frame_size) {
						LOG_DEBUG("Dropping too small frame sized %d bytes, assuming it as broken", buf_info.bytesused);
						goto pass_frame;
					}

					{
						long double now = now_ms_ld();

						if (now < grab_after) {
							fluency_passed += 1;
							LOG_PERF("Passed %u frames for fluency: now=%.03Lf; grab_after=%.03Lf", fluency_passed, now, grab_after);
							goto pass_frame;
						}

						fluency_passed = 0;
						if (log_level >= LOG_LEVEL_VERBOSE) {
							if ((long long)now != fps_second) {
								LOG_VERBOSE("Current FPS = %u", fps);
								fps = 0;
								fps_second = (long long)now;
							}
							++fps;
						}

						long double delay = _stream_get_fluency_delay(stream->dev, &pool);
						grab_after = now + delay;
						LOG_PERF("Fluency delay=%.03Lf; grab_after=%.03Lf", delay, grab_after);
					}

					LOG_DEBUG("Grabbed a new frame to buffer %d", buf_info.index);
					pool.workers[buf_info.index].ctx.buf_info = buf_info;

					if (!last_worker) {
						last_worker = &pool.workers[buf_info.index];
					} else {
						last_worker->order_next = &pool.workers[buf_info.index];
					}

					A_PTHREAD_M_LOCK(&pool.workers[buf_info.index].has_job_mutex);
					pool.workers[buf_info.index].has_job = true;
					A_PTHREAD_M_UNLOCK(&pool.workers[buf_info.index].has_job_mutex);
					A_PTHREAD_C_SIGNAL(&pool.workers[buf_info.index].has_job_cond);

					goto next_handlers; // Поток сам освободит буфер

					pass_frame:

					if (_stream_release_buffer(stream->dev, &buf_info) < 0) {
						break;
					}
				}

				next_handlers:

				if (FD_ISSET(stream->dev->run->fd, &write_fds)) {
					LOG_ERROR("Got unexpected writing event, seems device was disconnected");
					break;
				}

				if (FD_ISSET(stream->dev->run->fd, &error_fds)) {
					LOG_INFO("Got V4L2 event");
					if (_stream_handle_event(stream->dev) < 0) {
						break;
					}
				}
			}
		}

		A_PTHREAD_M_LOCK(&stream->mutex);
		stream->picture.size = 0;
		free(stream->picture.data);
		stream->online = false;
		A_PTHREAD_M_UNLOCK(&stream->mutex);
	}

	_stream_destroy_workers(stream->dev, &pool);
	_stream_control(stream->dev, false);
	device_close(stream->dev);
}

void stream_loop_break(struct stream_t *stream) {
	stream->dev->stop = 1;
}

static long double _stream_get_fluency_delay(struct device_t *dev, struct workers_pool_t *pool) {
	long double delay = 0;

	for (unsigned index = 0; index < dev->run->n_buffers; ++index) {
		A_PTHREAD_M_LOCK(&pool->workers[index].last_comp_time_mutex);
		if (pool->workers[index].last_comp_time > 0) {
			delay += pool->workers[index].last_comp_time;
		}
		A_PTHREAD_M_UNLOCK(&pool->workers[index].last_comp_time_mutex);
	}
	// Среднее арифметическое деленное на количество воркеров
	return delay / dev->run->n_buffers / dev->run->n_buffers;
}

static int _stream_init_loop(struct device_t *dev, struct workers_pool_t *pool) {
	int retval = -1;

	LOG_DEBUG("%s: *dev->stop = %d", __FUNCTION__, dev->stop);
	while (!dev->stop) {
		if ((retval = _stream_init(dev, pool)) < 0) {
			LOG_INFO("Sleeping %d seconds before new stream init ...", dev->error_timeout);
			sleep(dev->error_timeout);
		} else {
			break;
		}
	}
	return retval;
}

static int _stream_init(struct device_t *dev, struct workers_pool_t *pool) {
	SEP_INFO('=');

	_stream_destroy_workers(dev, pool);
	_stream_control(dev, false);
	device_close(dev);

	if (device_open(dev) < 0) {
		goto error;
	}
	if (_stream_control(dev, true) < 0) {
		goto error;
	}
	_stream_init_workers(dev, pool);

	return 0;

	error:
		device_close(dev);
		return -1;
}

static void _stream_init_workers(struct device_t *dev, struct workers_pool_t *pool) {
	LOG_DEBUG("Spawning %d workers ...", dev->run->n_buffers);

	*pool->workers_stop = false;
	A_CALLOC(pool->workers, dev->run->n_buffers);

	A_PTHREAD_M_INIT(&pool->has_free_workers_mutex);
	A_PTHREAD_C_INIT(&pool->has_free_workers_cond);

	for (unsigned index = 0; index < dev->run->n_buffers; ++index) {
		A_PTHREAD_M_INIT(&pool->workers[index].has_job_mutex);
		A_PTHREAD_C_INIT(&pool->workers[index].has_job_cond);

		pool->workers[index].ctx.index = index;
		pool->workers[index].ctx.dev = dev;
		pool->workers[index].ctx.dev_stop = (sig_atomic_t *volatile)&dev->stop;
		pool->workers[index].ctx.workers_stop = pool->workers_stop;

		pool->workers[index].ctx.last_comp_time_mutex = &pool->workers[index].last_comp_time_mutex;
		pool->workers[index].ctx.last_comp_time = &pool->workers[index].last_comp_time;

		pool->workers[index].ctx.has_job_mutex = &pool->workers[index].has_job_mutex;
		pool->workers[index].ctx.has_job = &pool->workers[index].has_job;
		pool->workers[index].ctx.has_job_cond = &pool->workers[index].has_job_cond;

		pool->workers[index].ctx.has_free_workers_mutex = &pool->has_free_workers_mutex;
		pool->workers[index].ctx.has_free_workers = &pool->has_free_workers;
		pool->workers[index].ctx.has_free_workers_cond = &pool->has_free_workers_cond;

		A_PTHREAD_CREATE(&pool->workers[index].tid,	_stream_worker_thread, (void *)&pool->workers[index].ctx);
	}
}

static void *_stream_worker_thread(void *v_ctx) {
	struct worker_context_t *ctx = (struct worker_context_t *)v_ctx;

	LOG_DEBUG("Hello! I am a worker #%d ^_^", ctx->index);

	while (!*ctx->dev_stop && !*ctx->workers_stop) {
		A_PTHREAD_M_LOCK(ctx->has_free_workers_mutex);
		*ctx->has_free_workers = true;
		A_PTHREAD_M_UNLOCK(ctx->has_free_workers_mutex);
		A_PTHREAD_C_SIGNAL(ctx->has_free_workers_cond);

		LOG_DEBUG("Worker %d waiting for a new job ...", ctx->index);
		A_PTHREAD_M_LOCK(ctx->has_job_mutex);
		A_PTHREAD_C_WAIT_TRUE(*ctx->has_job, ctx->has_job_cond, ctx->has_job_mutex);
		A_PTHREAD_M_UNLOCK(ctx->has_job_mutex);

		if (!*ctx->workers_stop) {
			unsigned long compressed;
			time_t start_sec;
			time_t stop_sec;
			long start_msec;
			long stop_msec;
			long double last_comp_time;

			now_ms(&start_sec, &start_msec);

			LOG_DEBUG("Worker %d compressing JPEG ...", ctx->index);

			compressed = jpeg_compress_buffer(ctx->dev, ctx->index);

			assert(!_stream_release_buffer(ctx->dev, &ctx->buf_info)); // FIXME
			*ctx->has_job = false;

			now_ms(&stop_sec, &stop_msec);
			if (start_sec <= stop_sec) {
				last_comp_time = (stop_sec - start_sec) + ((long double)(stop_msec - start_msec)) / 1000;
			} else {
				last_comp_time = 0;
			}

			A_PTHREAD_M_LOCK(ctx->last_comp_time_mutex);
			*ctx->last_comp_time = last_comp_time;
			A_PTHREAD_M_UNLOCK(ctx->last_comp_time_mutex);

			LOG_PERF("Compressed JPEG size=%ld; time=%0.3Lf (worker %d)", compressed, last_comp_time, ctx->index); // FIXME
		}
	}

	LOG_DEBUG("Bye-bye (worker %d)", ctx->index);
	return NULL;
}

static void _stream_destroy_workers(struct device_t *dev, struct workers_pool_t *pool) {
	if (pool->workers) {
		LOG_INFO("Destroying workers ...");

		*pool->workers_stop = true;
		for (unsigned index = 0; index < dev->run->n_buffers; ++index) {
			A_PTHREAD_M_LOCK(&pool->workers[index].has_job_mutex);
			pool->workers[index].has_job = true; // Final job: die
			A_PTHREAD_M_UNLOCK(&pool->workers[index].has_job_mutex);
			A_PTHREAD_C_SIGNAL(&pool->workers[index].has_job_cond);

			A_PTHREAD_JOIN(pool->workers[index].tid);
			A_PTHREAD_M_DESTROY(&pool->workers[index].has_job_mutex);
			A_PTHREAD_C_DESTROY(&pool->workers[index].has_job_cond);
		}

		A_PTHREAD_M_DESTROY(&pool->has_free_workers_mutex);
		A_PTHREAD_C_DESTROY(&pool->has_free_workers_cond);

		free(pool->workers);
	}
	pool->workers = NULL;
}

static int _stream_control(struct device_t *dev, const bool enable) {
	if (enable != dev->run->capturing) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		LOG_DEBUG("Calling ioctl(%s) ...", (enable ? "VIDIOC_STREAMON" : "VIDIOC_STREAMOFF"));
		if (xioctl(dev->run->fd, (enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF), &type) < 0) {
			LOG_PERROR("Unable to %s capturing", (enable ? "start" : "stop"));
			if (enable) {
				return -1;
			}
		}

		dev->run->capturing = enable;
		LOG_INFO("Capturing %s", (enable ? "started" : "stopped"));
	}
    return 0;
}

static int _stream_grab_buffer(struct device_t *dev, struct v4l2_buffer *buf_info) {
	MEMSET_ZERO_PTR(buf_info);
	buf_info->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf_info->memory = V4L2_MEMORY_MMAP;

	LOG_DEBUG("Calling ioctl(VIDIOC_DQBUF) ...");
	if (xioctl(dev->run->fd, VIDIOC_DQBUF, buf_info) < 0) {
		LOG_PERROR("Unable to dequeue buffer");
		return -1;
	}

	LOG_DEBUG("Got a new frame in buffer index=%d; bytesused=%d", buf_info->index, buf_info->bytesused);
	if (buf_info->index >= dev->run->n_buffers) {
		LOG_ERROR("Got invalid buffer index=%d; nbuffers=%d", buf_info->index, dev->run->n_buffers);
		return -1;
	}
	return 0;
}

static int _stream_release_buffer(struct device_t *dev, struct v4l2_buffer *buf_info) {
	LOG_DEBUG("Calling ioctl(VIDIOC_QBUF) ...");
	if (xioctl(dev->run->fd, VIDIOC_QBUF, buf_info) < 0) {
		LOG_PERROR("Unable to requeue buffer");
		return -1;
	}
	return 0;
}

static int _stream_handle_event(struct device_t *dev) {
	struct v4l2_event event;

	LOG_DEBUG("Calling ioctl(VIDIOC_DQEVENT) ...");
	if (!xioctl(dev->run->fd, VIDIOC_DQEVENT, &event)) {
		switch (event.type) {
			case V4L2_EVENT_SOURCE_CHANGE:
				LOG_INFO("Got V4L2_EVENT_SOURCE_CHANGE: source changed");
				return -1;
			case V4L2_EVENT_EOS:
				LOG_INFO("Got V4L2_EVENT_EOS: end of stream (ignored)");
				return 0;
		}
	} else {
		LOG_PERROR("Got some V4L2 device event, but where is it? ");
	}
	return 0;
}