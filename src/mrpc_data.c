#include "private/mrpc_common.h"

#include "private/mrpc_data.h"
#include "private/mrpc_method.h"
#include "private/mrpc_param.h"
#include "private/mrpc_interface.h"
#include "ff/ff_stream.h"

struct mrpc_data
{
	struct mrpc_method *method;
	struct mrpc_param **request_params;
	struct mrpc_param **response_params;
	uint8_t method_id;
};

static struct mrpc_data *try_create_mrpc_data(struct mrpc_interface *interface, uint8_t method_id)
{
	struct mrpc_method *method;
	struct mrpc_data *data = NULL;

	method = mrpc_interface_get_method(interface, method_id);
	if (method != NULL)
	{
		data = (struct mrpc_data *) ff_malloc(sizeof(*data));
		data->method = method;
		data->request_params = mrpc_method_create_request_params(method);
		data->response_params = mrpc_method_create_response_params(method);
		data->method_id = method_id;
	}

	return data;
}

static struct mrpc_data *read_request(struct mrpc_interface *interface, struct ff_stream *stream)
{
	uint8_t method_id;
	struct mrpc_data *data = NULL;
	int is_success;

	is_success = ff_stream_read(stream, &method_id, 1);
	if (is_success)
	{
		data = try_create_mrpc_data(interface, method_id);
		if (data != NULL)
		{
			is_success = mrpc_method_read_request_params(data->method, data->request_params, stream);
			if (!is_success)
			{
				mrpc_data_delete(data);
				data = NULL;
			}
		}
	}

	return data;
}

static int read_response(struct mrpc_data *data, struct ff_stream *stream)
{
	int is_success;

	is_success = mrpc_method_read_response_params(data->method, data->response_params, stream);
	return is_success;
}

static int write_response(struct mrpc_data *data, struct ff_stream *stream)
{
	int is_success;

	is_success = mrpc_method_write_response_params(data->method, data->response_params, stream);
	if (is_success)
	{
		is_success = ff_stream_flush(stream);
	}
	return is_success;
}

static int write_request(struct mrpc_data *data, struct ff_stream *stream)
{
	int is_success;

	is_success = ff_stream_write(stream, &data->method_id, 1);
	if (is_success)
	{
		is_succes = mrpc_method_write_request_params(data->method, data->request_params, stream);
		if (is_success)
		{
			is_success = ff_stream_flush(stream);
		}
	}

	return is_success;
}

struct mrpc_data *mrpc_data_create(struct mrpc_interface *interface, uint8_t method_id)
{
	struct mrpc_data *data;

	data = try_create_mrpc_data(interface, method_id);
	ff_assert(data != NULL);
	return data;
}

void mrpc_data_delete(struct mrpc_data *data)
{
	mrpc_method_delete_request_params(data->method, data->request_params);
	mrpc_method_delete_response_params(data->method, data->response_params);
	ff_free(data);
}

int mrpc_data_process_remote_call(struct mrpc_interface *interface, void *service_ctx, struct ff_stream *stream)
{
	struct mrpc_data *data;
	int is_success = 0;

	data = read_request(interface, stream);
	if (data != NULL)
	{
		mrpc_method_invoke_callback(data->method, data, service_ctx);
		is_success = write_response(data, stream);
		mrpc_data_delete(data);
    }

    return is_success;
}

int mrpc_data_invoke_remote_call(struct mrpc_data *data, struct ff_stream *stream)
{
	int is_success;

	is_success = write_request(data, stream);
	if (is_success)
	{
		is_success = read_response(data, stream);
	}

	return is_success;
}

void mrpc_data_get_request_param_value(struct mrpc_data *data, int param_idx, void **value)
{
	mrpc_method_get_request_param_value(data->method, param_idx, data->request_params, value);
}

void mrpc_data_get_response_param_value(struct mrpc_data *data, int param_idx, void **value)
{
	mrpc_method_get_response_param_value(data->method, param_idx, data->response_params, value);
}

void mrpc_data_set_request_param_value(struct mrpc_data *data, int param_idx, const void *value)
{
	mrpc_method_set_request_param_value(data->method, param_idx, data->request_params, value);
}

void mrpc_data_set_response_param_value(struct mrpc_data *data, int param_idx, const void *value)
{
	mrpc_method_set_response_param_value(data->method, param_idx, data->response_params, value);
}

uint32_t mrpc_data_get_request_hash(struct mrpc_data *data, uint32_t start_value)
{
	uint32_t hash_value;

	hash_value = mrpc_method_get_request_hash(data->method, start_value, data->request_params);
	return hash_value;
}