#include "private/mrpc_common.h"

#include "private/mrpc_client_stream_processor.h"
#include "private/mrpc_client_request_processor.h"
#include "private/mrpc_bitmap.h"
#include "ff/ff_blocking_queue.h"
#include "ff/ff_pool.h"
#include "ff/ff_event.h"
#include "ff/ff_stream.h"
#include "ff/ff_core.h"

/**
 * the maximum number of mrpc_client_request_processor instances, which can be simultaneously invoked
 * by the current mrpc_client_stream_processor.
 * This number should be less or equal to the 0x100, because of restriction on the size (1 bytes) of request_id
 * field in the mrpc client-server protocol. request_id is assigned for each currently running request
 * and is used for association with the corresponding asynchronous response.
 * So there are 0x100 maximum request processors can be invoked in parallel.
 */
#define MAX_REQUEST_PROCESSORS_CNT 0x100

/**
 * the maximum number of mrpc_packet packets pending in the writer queue.
 * these packets are written by the stream_writer_func to the underlying stream.
 * TODO: determine the optimal size of this parameter.
 */
#define MAX_WRITER_QUEUE_SIZE 500

/**
 * the maximum number of mrpc_packet packets, which can be used by the instance of the
 * mrpc_client_stream_processor. These packets are used when receiving data from the underlying stream.
 * Also they are used by the mrpc_client_request_processor when serializing rpc requests in the
 * mrpc_client_request_processor_invoke_rpc().
 * Note that these packets are allocated on demand using the ff_pool, so the MAX_PACKETS_CNT value can be quite high
 * without sacrificing available memory.
 * TODO: determine the optimal size of this parameter.
 */
#define MAX_PACKETS_CNT 1000

struct mrpc_client_stream_processor
{
	struct ff_blocking_queue *writer_queue;
	struct ff_event *writer_stop_event;
	struct ff_pool *request_processors_pool;
	struct ff_event *request_processors_stop_event;
	struct mrpc_bitmap *request_processors_bitmap;
	struct ff_pool *packets_pool;
	struct mrpc_client_request_processor *active_request_processors[MAX_REQUEST_PROCESSORS_CNT];
	struct ff_stream *stream;
	int active_request_processors_cnt;
};

static void skip_writer_queue_packets(struct mrpc_client_stream_processor *stream_processor)
{
	for (;;)
	{
		struct mrpc_packet *packet;

		ff_blocking_queue_get(stream_processor->writer_queue, &packet);
		if (packet == NULL)
		{
			int is_empty;

			is_empty = ff_blocking_queue_is_empty(stream_processor->writer_queue);
			ff_assert(is_empty);
			break;
		}
		ff_pool_release_entry(stream_processor->packets_pool, packet);
	}
}

static void stream_writer_func(void *ctx)
{
	struct mrpc_client_stream_processor *stream_processor;

	stream_processor = (struct mrpc_client_stream_processor *) ctx;
	ff_assert(stream_processor->stream != NULL);
	for (;;)
	{
		struct mrpc_packet *packet;
		int is_empty;
		enum ff_result result;

		ff_blocking_queue_get(stream_processor->writer_queue, &packet);
		if (packet == NULL)
		{
			is_empty = ff_blocking_queue_is_empty(stream_processor->writer_queue);
			ff_assert(is_empty);
			break;
		}
		result = mrpc_packet_write_to_stream(packet, stream_processor->stream);
		ff_pool_release_entry(stream_processor->packets_pool, packet);

		/* below is an optimization, which is used for minimizing the number of
		 * usually expensive ff_stream_flush() calls. These calls are invoked only
		 * if the writer_queue is empty at the moment. If we won't flush the stream
		 * this moment moment, then potential deadlock can occur:
		 * 1) client serializes rpc request into the mrpc_packets and pushs them into the writer_queue.
		 * 2) this function writes these packets into the stream.
		 *    Usually this means that the packets' content is buffered in the underlying stream buffer
		 *    and not sent to the remote side (i.e. server).
		 * 3) this function blocks awaiting for the new packets in the writer_packet.
		 * 4) deadlock:
		 *     - server is blocked awaiting for request from the client, which is still buffered on the client side;
		 *     - client is blocked awaiting for response from the server.
		 */
		is_empty = ff_blocking_queue_is_empty(stream_processor->writer_queue);
		if (result == FF_SUCCESS && is_empty)
		{
			result = ff_stream_flush(stream_processor->stream);
		}
		if (result != FF_SUCCESS)
		{
			mrpc_client_stream_processor_stop_async(stream_processor);
			skip_writer_queue_packets(stream_processor);
			break;
		}
	}
	ff_event_set(stream_processor->writer_stop_event);
}

static struct mrpc_packet *acquire_packet(void *ctx)
{
	struct mrpc_client_stream_processor *stream_processor;
	struct mrpc_packet *packet;

	stream_processor = (struct mrpc_client_stream_processor *) ctx;
	packet = (struct mrpc_packet *) ff_pool_acquire_entry(stream_processor->packets_pool);
	return packet;
}

static void release_packet(void *ctx, struct mrpc_packet *packet)
{
	struct mrpc_client_stream_processor *stream_processor;

	stream_processor = (struct mrpc_client_stream_processor *) ctx;
	mrpc_packet_reset(packet);
	ff_pool_release_entry(stream_processor->packets_pool, packet);
}

static uint8_t acquire_request_id(struct mrpc_client_stream_processor *stream_processor)
{
	int request_id;

	request_id = mrpc_bitmap_acquire_bit(stream_processor->request_processors_bitmap);
	ff_assert(request_id >= 0);
	ff_assert(request_id < MAX_REQUEST_PROCESSORS_CNT);

	return (uint8_t) request_id;
}

static void release_request_id(void *ctx, uint8_t request_id)
{
	struct mrpc_client_stream_processor *stream_processor;

	stream_processor = (struct mrpc_client_stream_processor *) ctx;
	mrpc_bitmap_release_bit(stream_processor->request_processors_bitmap, request_id);
}

static void *create_request_processor(void *ctx)
{
	struct mrpc_client_stream_processor *stream_processor;
	struct mrpc_client_request_processor *request_processor;
	uint8_t request_id;

	stream_processor = (struct mrpc_client_stream_processor *) ctx;

	request_id = acquire_request_id(stream_processor);
	request_processor = mrpc_client_request_processor_create(acquire_packet, release_packet, stream_processor, release_request_id, stream_processor,
		stream_processor->writer_queue, request_id);
	return request_processor;
}

static void delete_request_processor(void *ctx)
{
	struct mrpc_client_request_processor *request_processor;

	request_processor = (struct mrpc_client_request_processor *) ctx;
	mrpc_client_request_processor_delete(request_processor);
}

static struct mrpc_client_request_processor *acquire_request_processor(struct mrpc_client_stream_processor *stream_processor)
{
	struct mrpc_client_request_processor *request_processor;
	uint8_t request_id;

	ff_assert(stream_processor->active_request_processors_cnt >= 0);
	ff_assert(stream_processor->active_request_processors_cnt <= MAX_REQUEST_PROCESSORS_CNT);

	request_processor = (struct mrpc_client_request_processor *) ff_pool_acquire_entry(stream_processor->request_processors_pool);
	request_id = mrpc_client_request_processor_get_request_id(request_processor);
	ff_assert(stream_processor->active_request_processors[request_id] == NULL);
	stream_processor->active_request_processors[request_id] = request_processor;

	stream_processor->active_request_processors_cnt++;
	ff_assert(stream_processor->active_request_processors_cnt <= MAX_REQUEST_PROCESSORS_CNT);
	if (stream_processor->active_request_processors_cnt == 1)
	{
		ff_event_reset(stream_processor->request_processors_stop_event);
	}

	return request_processor;
}

static void release_request_processor(struct mrpc_client_stream_processor *stream_processor, struct mrpc_client_request_processor *request_processor)
{
	uint8_t request_id;

	ff_assert(stream_processor->active_request_processors_cnt > 0);
	ff_assert(stream_processor->active_request_processors_cnt <= MAX_REQUEST_PROCESSORS_CNT);

	request_id = mrpc_client_request_processor_get_request_id(request_processor);
	ff_assert(stream_processor->active_request_processors[request_id] == request_processor);
	stream_processor->active_request_processors[request_id] = NULL;
	ff_pool_release_entry(stream_processor->request_processors_pool, request_processor);

	stream_processor->active_request_processors_cnt--;
	if (stream_processor->active_request_processors_cnt == 0)
	{
		ff_event_set(stream_processor->request_processors_stop_event);
	}
}

static void *create_packet(void *ctx)
{
	struct mrpc_packet *packet;

	packet = mrpc_packet_create();
	return packet;
}

static void delete_packet(void *ctx)
{
	struct mrpc_packet *packet;

	packet = (struct mrpc_packet *) ctx;
	mrpc_packet_delete(packet);
}

static void stop_request_processor(void *entry, void *ctx, int is_acquired)
{
	struct mrpc_client_request_processor *request_processor;

	request_processor = (struct mrpc_client_request_processor *) entry;
	if (is_acquired)
	{
		mrpc_client_request_processor_stop_async(request_processor);
	}
}

static void stop_all_request_processors(struct mrpc_client_stream_processor *stream_processor)
{
	ff_pool_for_each_entry(stream_processor->request_processors_pool, stop_request_processor, stream_processor);
	ff_event_wait(stream_processor->request_processors_stop_event);
	ff_assert(stream_processor->active_request_processors_cnt == 0);
}

static void start_stream_writer(struct mrpc_client_stream_processor *stream_processor)
{
	ff_core_fiberpool_execute_async(stream_writer_func, stream_processor);
}

static void stop_stream_writer(struct mrpc_client_stream_processor *stream_processor)
{
	ff_blocking_queue_put(stream_processor->writer_queue, NULL);
	ff_event_wait(stream_processor->writer_stop_event);
}

struct mrpc_client_stream_processor *mrpc_client_stream_processor_create()
{
	struct mrpc_client_stream_processor *stream_processor;
	int i;

	stream_processor = (struct mrpc_client_stream_processor *) ff_malloc(sizeof(*stream_processor));
	stream_processor->writer_queue = ff_blocking_queue_create(MAX_WRITER_QUEUE_SIZE);
	stream_processor->writer_stop_event = ff_event_create(FF_EVENT_AUTO);
	stream_processor->request_processors_pool = ff_pool_create(MAX_REQUEST_PROCESSORS_CNT, create_request_processor, stream_processor, delete_request_processor);
	stream_processor->request_processors_stop_event = ff_event_create(FF_EVENT_AUTO);
	stream_processor->request_processors_bitmap = mrpc_bitmap_create(MAX_REQUEST_PROCESSORS_CNT);
	stream_processor->packets_pool = ff_pool_create(MAX_PACKETS_CNT, create_packet, stream_processor, delete_packet);

	for (i = 0; i < MAX_REQUEST_PROCESSORS_CNT; i++)
	{
		stream_processor->active_request_processors[i] = NULL;
	}

	stream_processor->stream = NULL;
	stream_processor->active_request_processors_cnt = 0;

	return stream_processor;
}

void mrpc_client_stream_processor_delete(struct mrpc_client_stream_processor *stream_processor)
{
	ff_assert(stream_processor->stream == NULL);
	ff_assert(stream_processor->active_request_processors_cnt == 0);

	ff_pool_delete(stream_processor->packets_pool);
	mrpc_bitmap_delete(stream_processor->request_processors_bitmap);
	ff_event_delete(stream_processor->request_processors_stop_event);
	ff_pool_delete(stream_processor->request_processors_pool);
	ff_event_delete(stream_processor->writer_stop_event);
	ff_blocking_queue_delete(stream_processor->writer_queue);
	ff_free(stream_processor);
}

void mrpc_client_stream_processor_process_stream(struct mrpc_client_stream_processor *stream_processor, struct ff_stream *stream)
{
	ff_assert(stream_processor->stream == NULL);
	ff_assert(stream_processor->active_request_processors_cnt == 0);

	stream_processor->stream = stream;
	start_stream_writer(stream_processor);
	ff_event_set(stream_processor->request_processors_stop_event);

	for (;;)
	{
		struct mrpc_packet *packet;
		struct mrpc_client_request_processor *request_processor;
		uint8_t request_id;
		enum ff_result result;

		packet = (struct mrpc_packet *) ff_pool_acquire_entry(stream_processor->packets_pool);
		result = mrpc_packet_read_from_stream(packet, stream);
		if (result != FF_SUCCESS)
		{
			ff_pool_release_entry(stream_processor->packets_pool, packet);
			break;
		}

		request_id = mrpc_packet_get_request_id(packet);
		request_processor = stream_processor->active_request_processors[request_id];
		if (request_processor == NULL)
		{
			ff_pool_release_entry(stream_processor->packets_pool, packet);
			break;
		}
		mrpc_client_request_processor_push_packet(request_processor, packet);
	}

	mrpc_client_stream_processor_stop_async(stream_processor);
	stop_all_request_processors(stream_processor);
	ff_assert(stream_processor->active_request_processors_cnt == 0);
	stop_stream_writer(stream_processor);
}

void mrpc_client_stream_processor_stop_async(struct mrpc_client_stream_processor *stream_processor)
{
	if (stream_processor->stream != NULL)
	{
		ff_stream_disconnect(stream_processor->stream);
		stream_processor->stream = NULL;
	}
}

enum ff_result mrpc_client_stream_processor_invoke_rpc(struct mrpc_client_stream_processor *stream_processor, struct mrpc_data *data)
{
	enum ff_result result = FF_FAILURE;

	if (stream_processor->stream != NULL)
	{
		struct mrpc_client_request_processor *request_processor;

		request_processor = acquire_request_processor(stream_processor);
		result = mrpc_client_request_processor_invoke_rpc(request_processor, data);
		release_request_processor(stream_processor, request_processor);
	}

	return result;
}
