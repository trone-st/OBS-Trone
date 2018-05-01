/******************************************************************************
    Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <assert.h>
#include <inttypes.h>
#include "../util/bmem.h"
#include "../util/platform.h"
#include "../util/profiler.h"
#include "../util/threading.h"
#include "../util/darray.h"

#include "format-conversion.h"
#include "video-io.h"
#include "video-frame.h"
#include "video-scaler.h"

extern profiler_name_store_t *obs_get_profiler_name_store(void);

#define MAX_CONVERT_BUFFERS 3
#define MAX_CACHE_SIZE 16

struct cached_frame_info {
	struct video_data frame;
	int skipped;
	int count;
};

struct video_input {
	struct video_scale_info   conversion;
	video_scaler_t            *scaler;
	struct video_frame        frame[MAX_CONVERT_BUFFERS];
	int                       cur_frame;

	void (*callback)(void *param, struct video_data *frame);
	void *param;
};

static inline void video_input_free(struct video_input *input)
{
	for (size_t i = 0; i < MAX_CONVERT_BUFFERS; i++)
		video_frame_free(&input->frame[i]);
	video_scaler_destroy(input->scaler);
}

struct video_output {
	struct video_output_info   info;

	pthread_t                  thread;
	pthread_mutex_t            data_mutex;
	bool                       stop;

	os_sem_t                   *update_semaphore;
	uint64_t                   frame_time;
	uint32_t                   skipped_frames;
	uint32_t                   total_frames;

	bool                       initialized;

	pthread_mutex_t            input_mutex;
	DARRAY(struct video_input) inputs;

	size_t                     available_frames;
	size_t                     first_added;
	size_t                     last_added;
	struct cached_frame_info   cache[MAX_CACHE_SIZE];
};

/* ------------------------------------------------------------------------- */

static inline bool scale_video_output(struct video_input *input,
		struct video_data *data)
{
	bool success = true;

	if (input->scaler) {
		struct video_frame *frame;

		if (++input->cur_frame == MAX_CONVERT_BUFFERS)
			input->cur_frame = 0;

		frame = &input->frame[input->cur_frame];

		success = video_scaler_scale(input->scaler,
				frame->data, frame->linesize,
				(const uint8_t * const*)data->data,
				data->linesize);

		if (success) {
			for (size_t i = 0; i < MAX_AV_PLANES; i++) {
				data->data[i]     = frame->data[i];
				data->linesize[i] = frame->linesize[i];
			}
		} else {
			blog(LOG_WARNING, "video-io: Could not scale frame!");
		}
	}

	return success;
}

//size.total = size.width * size.height;
//y = yuv[position.y * size.width + position.x];
//u = yuv[(position.y / 2) * (size.width / 2) + (position.x / 2) + size.total];
//v = yuv[(position.y / 2) * (size.width / 2) + (position.x / 2) + size.total + (size.total / 4)];
//rgb = Y¡¬UV444toRGB888(y, u, v);
uint8_t clamp(int val, int min_val, int max_val) {
	if (val < min_val) return min_val;
	if (val > max_val) return max_val;
	return val;
}
uint8_t clip(int val) {
	if (val < 0) return 0;
	if (val > 255) return 255;
	return val;
}
void yuv2rgb(uint8_t yValue, uint8_t uValue, uint8_t vValue, uint8_t *r, uint8_t *g, uint8_t *b) {
	int rTmp = yValue + (1.370705 * (vValue - 128));
	int gTmp = yValue - (0.698001 * (vValue - 128)) - (0.337633 * (uValue - 128));
	int bTmp = yValue + (1.732446 * (uValue - 128));
	*r = clamp(rTmp, 0, 255);
	*g = clamp(gTmp, 0, 255);
	*b = clamp(bTmp, 0, 255);
}


static bool SaveFrameToFile(struct video_data *data) {

	//convert from NV12 format to uvyx format
	uint8_t           *ptr ;

	char *saveFilePathName = get_save_file_path_name();
	int width = data->linesize[0];
	int height = width * 9 / 16;
	ptr = bmalloc(3 * width * height);
	uint8_t A,Y, U, V, R, G, B;
	int  C, D, E;
	//convert from 4:2:0 -> 4:2:2
	uint8_t *chorma_out = bmalloc(width * height);
	uint8_t *chorma_in = data->data[1];
	int width_d2 = width / 2;
	int height_d2 = height / 2;
	for (int y = 0; y < height_d2; y++) {
		for (int x = 0; x < width; x++) {

				if (y < height_d2 - 2) {
					int A, B, C;
					A = chorma_in[y  * width_d2 + x];
					B = chorma_in[(y + 1)  * width_d2 + x];
					C = chorma_in[(y + 2)  * width_d2 + x];
					chorma_out[2*y*width_d2 + x] = A;
					chorma_out[(2*y + 1)*width / 2 + x] = clip(((9*(A+B))-(A+C)+8) >> 4);
				}
				else if (y == height_d2 - 2) {
					int R, Q, S;
					R = chorma_in[y  * width_d2 + x];
					Q = chorma_in[(y - 1)  * width_d2 + x];
					S = chorma_in[(y + 1)  * width_d2 + x];
					chorma_out[2*y*width_d2 + x] = R;
					chorma_out[(2*y + 1)*width / 2 + x] = clip(((9 * (R + S)) - (Q + S) + 8) >> 4);
				}
				else {
					int R, S;
					S = chorma_in[y  * width_d2 + x];
					R = chorma_in[(y - 1)  * width_d2 + x];
					chorma_out[2*y*width_d2 + x] = S;
					chorma_out[(2*y + 1)*width / 2 + x] = clip(((9 * (S + S)) - (R + S) + 8) >> 4);
				}
		}
	}
	//convert 4:2:2 -> 4:4:4
		for (int y = 0; y < height_d2; y++) {
		for (int x = 0; x < width; x++) {

				if (y < height_d2 - 2) {
					int A, B, C;
					A = chorma_in[y  * width_d2 + x];
					B = chorma_in[(y + 1)  * width_d2 + x];
					C = chorma_in[(y + 2)  * width_d2 + x];
					chorma_out[2*y*width_d2 + x] = A;
					chorma_out[(2*y + 1)*width / 2 + x] = clip(((9*(A+B))-(A+C)+8) >> 4);
				}
				else if (y == height_d2 - 2) {
					int R, Q, S;
					R = chorma_in[y  * width_d2 + x];
					Q = chorma_in[(y - 1)  * width_d2 + x];
					S = chorma_in[(y + 1)  * width_d2 + x];
					chorma_out[2*y*width_d2 + x] = R;
					chorma_out[(2*y + 1)*width / 2 + x] = clip(((9 * (R + S)) - (Q + S) + 8) >> 4);
				}
				else {
					int R, S;
					S = chorma_in[y  * width_d2 + x];
					R = chorma_in[(y - 1)  * width_d2 + x];
					chorma_out[2*y*width_d2 + x] = S;
					chorma_out[(2*y + 1)*width / 2 + x] = clip(((9 * (S + S)) - (R + S) + 8) >> 4);
				}
		}
	}
	uint8_t *chorma_result = bmalloc(width * height*2);
	int total = width * height;


	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			Y = data->data[0][y * width + x];
			U = data->data[1][(y/2 ) * (width/2)*2 + (x/2)*2];
			V = data->data[1][(y/2 ) * (width/2)*2 + (x/2)*2 + 1];

			C = Y - 16;
			D = U - 128;
			E = V - 128;
			R = clip((298 * C + 409 * E + 128) >> 8);
			G = clip((298 * C - 100 * D - 208 * E + 128) >> 8);
			B = clip((298 * C + 516 * D + 128) >> 8);
			ptr[3 * y*width + 3 * x + 0] = B;
			ptr[3 * y*width + 3 * x + 1] = G;
			ptr[3 * y*width + 3 * x + 2] = R;

		}
	}
	image_write(ptr, saveFilePathName,data->linesize[0],height);
	return true;
}

static inline bool video_output_cur_frame(struct video_output *video)
{
	struct cached_frame_info *frame_info;
	bool complete;
	bool skipped;

	/* -------------------------------- */

	pthread_mutex_lock(&video->data_mutex);

	frame_info = &video->cache[video->first_added];
	
	if (get_screen_capture_active()) {
		struct video_data frame = frame_info->frame;
		SaveFrameToFile(&frame);
		set_screen_capture_active(false);
		
	}
	
	pthread_mutex_unlock(&video->data_mutex);

	/* -------------------------------- */

	pthread_mutex_lock(&video->input_mutex);
	// added by kgc 
	bool breaked = false;
	for (size_t i = 0; i < video->inputs.num; i++) {
		struct video_input *input = video->inputs.array+i;
		struct video_data frame = frame_info->frame;
		
		if (get_recording_paused()) {
			breaked = true;
			break;
		}
		
		if (scale_video_output(input, &frame))
			input->callback(input->param, &frame);
	}

	pthread_mutex_unlock(&video->input_mutex);

	/* -------------------------------- */

	pthread_mutex_lock(&video->data_mutex);
	
	if (breaked)
	{
		complete = true;
	}
	
	frame_info->frame.timestamp += video->frame_time;
	complete = --frame_info->count == 0;
	skipped = frame_info->skipped > 0;

	if (complete) {
		if (++video->first_added == video->info.cache_size)
			video->first_added = 0;

		if (++video->available_frames == video->info.cache_size)
			video->last_added = video->first_added;
	} else if (skipped) {
		--frame_info->skipped;
		++video->skipped_frames;
	}

	pthread_mutex_unlock(&video->data_mutex);

	/* -------------------------------- */

	return complete;
}

static void *video_thread(void *param)
{
	struct video_output *video = param;

	os_set_thread_name("video-io: video thread");

	const char *video_thread_name =
		profile_store_name(obs_get_profiler_name_store(),
				"video_thread(%s)", video->info.name);

	while (os_sem_wait(video->update_semaphore) == 0) {
		if (video->stop)
			break;

		profile_start(video_thread_name);
		while (!video->stop && !video_output_cur_frame(video)) {
			video->total_frames++;
		}

		video->total_frames++;
		profile_end(video_thread_name);

		profile_reenable_thread();
	}

	return NULL;
}

/* ------------------------------------------------------------------------- */

static inline bool valid_video_params(const struct video_output_info *info)
{
	return info->height != 0 && info->width != 0 && info->fps_den != 0 &&
	       info->fps_num != 0;
}

static inline void init_cache(struct video_output *video)
{
	if (video->info.cache_size > MAX_CACHE_SIZE)
		video->info.cache_size = MAX_CACHE_SIZE;

	for (size_t i = 0; i < video->info.cache_size; i++) {
		struct video_frame *frame;
		frame = (struct video_frame*)&video->cache[i];

		video_frame_init(frame, video->info.format,
				video->info.width, video->info.height);
	}

	video->available_frames = video->info.cache_size;
}

int video_output_open(video_t **video, struct video_output_info *info)
{
	struct video_output *out;
	pthread_mutexattr_t attr;

	if (!valid_video_params(info))
		return VIDEO_OUTPUT_INVALIDPARAM;

	out = bzalloc(sizeof(struct video_output));
	if (!out)
		goto fail;

	memcpy(&out->info, info, sizeof(struct video_output_info));
	out->frame_time = (uint64_t)(1000000000.0 * (double)info->fps_den /
		(double)info->fps_num);
	out->initialized = false;

	if (pthread_mutexattr_init(&attr) != 0)
		goto fail;
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0)
		goto fail;
	if (pthread_mutex_init(&out->data_mutex, &attr) != 0)
		goto fail;
	if (pthread_mutex_init(&out->input_mutex, &attr) != 0)
		goto fail;
	if (os_sem_init(&out->update_semaphore, 0) != 0)
		goto fail;
	if (pthread_create(&out->thread, NULL, video_thread, out) != 0)
		goto fail;

	init_cache(out);

	out->initialized = true;
	*video = out;
	return VIDEO_OUTPUT_SUCCESS;

fail:
	video_output_close(out);
	return VIDEO_OUTPUT_FAIL;
}

void video_output_close(video_t *video)
{
	if (!video)
		return;

	video_output_stop(video);

	for (size_t i = 0; i < video->inputs.num; i++)
		video_input_free(&video->inputs.array[i]);
	da_free(video->inputs);

	for (size_t i = 0; i < video->info.cache_size; i++)
		video_frame_free((struct video_frame*)&video->cache[i]);

	os_sem_destroy(video->update_semaphore);
	pthread_mutex_destroy(&video->data_mutex);
	pthread_mutex_destroy(&video->input_mutex);
	bfree(video);
}

static size_t video_get_input_idx(const video_t *video,
		void (*callback)(void *param, struct video_data *frame),
		void *param)
{
	for (size_t i = 0; i < video->inputs.num; i++) {
		struct video_input *input = video->inputs.array+i;
		if (input->callback == callback && input->param == param)
			return i;
	}

	return DARRAY_INVALID;
}

static inline bool video_input_init(struct video_input *input,
		struct video_output *video)
{
	if (input->conversion.width  != video->info.width ||
	    input->conversion.height != video->info.height ||
	    input->conversion.format != video->info.format) {
		struct video_scale_info from = {
			.format = video->info.format,
			.width  = video->info.width,
			.height = video->info.height,
			.range = video->info.range,
			.colorspace = video->info.colorspace
		};

		int ret = video_scaler_create(&input->scaler,
				&input->conversion, &from,
				VIDEO_SCALE_FAST_BILINEAR);
		if (ret != VIDEO_SCALER_SUCCESS) {
			if (ret == VIDEO_SCALER_BAD_CONVERSION)
				blog(LOG_ERROR, "video_input_init: Bad "
				                "scale conversion type");
			else
				blog(LOG_ERROR, "video_input_init: Failed to "
				                "create scaler");

			return false;
		}

		for (size_t i = 0; i < MAX_CONVERT_BUFFERS; i++)
			video_frame_init(&input->frame[i],
					input->conversion.format,
					input->conversion.width,
					input->conversion.height);
	}

	return true;
}

bool video_output_connect(video_t *video,
		const struct video_scale_info *conversion,
		void (*callback)(void *param, struct video_data *frame),
		void *param)
{
	bool success = false;

	if (!video || !callback)
		return false;

	pthread_mutex_lock(&video->input_mutex);

	if (video->inputs.num == 0) {
		video->skipped_frames = 0;
		video->total_frames = 0;
	}

	if (video_get_input_idx(video, callback, param) == DARRAY_INVALID) {
		struct video_input input;
		memset(&input, 0, sizeof(input));

		input.callback = callback;
		input.param    = param;

		if (conversion) {
			input.conversion = *conversion;
		} else {
			input.conversion.format    = video->info.format;
			input.conversion.width     = video->info.width;
			input.conversion.height    = video->info.height;
		}

		if (input.conversion.width == 0)
			input.conversion.width = video->info.width;
		if (input.conversion.height == 0)
			input.conversion.height = video->info.height;

		success = video_input_init(&input, video);
		if (success)
			da_push_back(video->inputs, &input);
	}

	pthread_mutex_unlock(&video->input_mutex);

	return success;
}

void video_output_disconnect(video_t *video,
		void (*callback)(void *param, struct video_data *frame),
		void *param)
{
	if (!video || !callback)
		return;

	pthread_mutex_lock(&video->input_mutex);

	size_t idx = video_get_input_idx(video, callback, param);
	if (idx != DARRAY_INVALID) {
		video_input_free(video->inputs.array+idx);
		da_erase(video->inputs, idx);
	}

	if (video->inputs.num == 0) {
		double percentage_skipped = (double)video->skipped_frames /
			(double)video->total_frames * 100.0;

		if (video->skipped_frames)
			blog(LOG_INFO, "Video stopped, number of "
					"skipped frames due "
					"to encoding lag: "
					"%"PRIu32"/%"PRIu32" (%0.1f%%)",
					video->skipped_frames,
					video->total_frames,
					percentage_skipped);
	}

	pthread_mutex_unlock(&video->input_mutex);
}

bool video_output_active(const video_t *video)
{
	if (!video) return false;
	return video->inputs.num != 0;
}

const struct video_output_info *video_output_get_info(const video_t *video)
{
	return video ? &video->info : NULL;
}

bool video_output_lock_frame(video_t *video, struct video_frame *frame,
		int count, uint64_t timestamp)
{
	struct cached_frame_info *cfi;
	bool locked;

	if (!video) return false;

	pthread_mutex_lock(&video->data_mutex);

	if (video->available_frames == 0) {
		video->cache[video->last_added].count += count;
		video->cache[video->last_added].skipped += count;
		locked = false;

	} else {
		if (video->available_frames != video->info.cache_size) {
			if (++video->last_added == video->info.cache_size)
				video->last_added = 0;
		}

		cfi = &video->cache[video->last_added];
		cfi->frame.timestamp = timestamp;
		cfi->count = count;
		cfi->skipped = 0;

		memcpy(frame, &cfi->frame, sizeof(*frame));

		locked = true;
	}

	pthread_mutex_unlock(&video->data_mutex);

	return locked;
}

void video_output_unlock_frame(video_t *video)
{
	if (!video) return;

	pthread_mutex_lock(&video->data_mutex);

	video->available_frames--;
	os_sem_post(video->update_semaphore);

	pthread_mutex_unlock(&video->data_mutex);
}

uint64_t video_output_get_frame_time(const video_t *video)
{
	return video ? video->frame_time : 0;
}

void video_output_stop(video_t *video)
{
	void *thread_ret;

	if (!video)
		return;

	if (video->initialized) {
		video->initialized = false;
		video->stop = true;
		os_sem_post(video->update_semaphore);
		pthread_join(video->thread, &thread_ret);
	}
}

bool video_output_stopped(video_t *video)
{
	if (!video)
		return true;

	return video->stop;
}

enum video_format video_output_get_format(const video_t *video)
{
	return video ? video->info.format : VIDEO_FORMAT_NONE;
}

uint32_t video_output_get_width(const video_t *video)
{
	return video ? video->info.width : 0;
}

uint32_t video_output_get_height(const video_t *video)
{
	return video ? video->info.height : 0;
}

double video_output_get_frame_rate(const video_t *video)
{
	if (!video)
		return 0.0;

	return (double)video->info.fps_num / (double)video->info.fps_den;
}

uint32_t video_output_get_skipped_frames(const video_t *video)
{
	return video->skipped_frames;
}

uint32_t video_output_get_total_frames(const video_t *video)
{
	return video->total_frames;
}

bool screen_capturing_active = false;
char save_file_path_name[1024] = "";
bool get_screen_capture_active() {
	return screen_capturing_active;
}
bool set_screen_capture_active(bool active) {
	screen_capturing_active = active;
	return screen_capturing_active;
}
const char * get_save_file_path_name() 
{
	return save_file_path_name;
}
void set_save_file_path_name(const char *path_name) {
	strcpy_s(save_file_path_name, 1024, path_name);
}
bool recordingPaused = false;
bool get_recording_paused() {
	return recordingPaused;
}
bool set_recording_paused(bool bPaused) {
	recordingPaused = bPaused;
	return recordingPaused;
}
