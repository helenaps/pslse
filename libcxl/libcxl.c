/*
 * Copyright 2014,2015 International Business Machines
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "libcxl.h"
#include "libcxl_internal.h"
#include "../common/utils.h"

#define API_VERSION            1
#define API_VERSION_COMPATIBLE 1

#ifdef DEBUG
#define DPRINTF(...) printf(__VA_ARGS__)
#else
#define DPRINTF(...)
#endif

/*
 * System constants
 */

#define MAX_LINE_CHARS 1024

#define FOURK_MASK        0xFFFFFFFFFFFFF000L

#define DSISR 0x4000000040000000L

static void _delay_1ms() {
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;
	nanosleep(&ts, &ts);
}

static int _testmemaddr(uint8_t * memaddr)
{
	int fd[2];
	int ret = 0;
	if (pipe(fd) >= 0) {
		if (write(fd[1], memaddr, 1) > 0)
			ret = 1;
	}

	close(fd[0]);
	close(fd[1]);

	return ret;
}

static void _handle_dsi(struct cxl_afu_h *afu, uint64_t addr)
{
	uint16_t size;
	uint8_t type;

	// Only track a single DSI at a time
	if (afu->dsi != NULL)
		return;

	size = sizeof(struct cxl_event_header) +
	       sizeof(struct cxl_event_data_storage);
	afu->dsi = (struct cxl_event*) calloc(1, size);
	afu->dsi->header.type = CXL_EVENT_DATA_STORAGE;
	afu->dsi->header.size = size;
	afu->dsi->header.process_element = afu->context;
	afu->dsi->fault.addr = addr & FOURK_MASK;
	afu->dsi->fault.dsisr = DSISR;
	if (afu->first_event == NULL)
		afu->first_event = afu->dsi;
	type = (uint8_t) CXL_EVENT_DATA_STORAGE;
	// FIXME: Handle errors on write?
	write(afu->pipe, &type, 1);
}

static void _handle_read(struct cxl_afu_h *afu, uint64_t addr, uint8_t size)
{
	uint8_t buffer[MAX_LINE_CHARS];

	if (!_testmemaddr((uint8_t *) addr)) {
		_handle_dsi(afu, addr);
		DPRINTF("READ from invalid addr @ 0x%016"PRIx64"\n", addr);
		buffer[0] = (uint8_t) PSLSE_MEM_FAILURE;
		if (put_bytes_silent(afu->fd, 1, buffer) != 1) {
			afu->opened = 0;
			afu->attached = 0;
		}
		return;
	}
	buffer[0] = PSLSE_MEM_SUCCESS;
	memcpy(&(buffer[1]), (void *) addr, size);
	if (put_bytes_silent(afu->fd, size+1, buffer) != size+1) {
		afu->opened = 0;
		afu->attached = 0;
	}
	DPRINTF("READ from addr @ 0x%016"PRIx64"\n", addr);
}

static void _handle_write(struct cxl_afu_h *afu, uint64_t addr, uint8_t size,
			 uint8_t *data)
{
	uint8_t buffer;

	if (!_testmemaddr((uint8_t *) addr)) {
		_handle_dsi(afu, addr);
		DPRINTF("WRITE to invalid addr @ 0x%016"PRIx64"\n", addr);
		buffer = PSLSE_MEM_FAILURE;
		if (put_bytes_silent(afu->fd, 1, &buffer) != 1) {
			afu->opened = 0;
			afu->attached = 0;
		}
		return;
	}
	memcpy((void *) addr, data, size);
	buffer = PSLSE_MEM_SUCCESS;
	if (put_bytes_silent(afu->fd, 1, &buffer) != 1) {
		afu->opened = 0;
		afu->attached = 0;
	}
	DPRINTF("WRITE to addr @ 0x%016"PRIx64"\n", addr);
}

static void _handle_touch(struct cxl_afu_h *afu, uint64_t addr, uint8_t size)
{
	uint8_t buffer;

	if (!_testmemaddr((uint8_t *) addr)) {
		_handle_dsi(afu, addr);
		DPRINTF("TOUCH of invalid addr @ 0x%016"PRIx64"\n", addr);
		buffer = (uint8_t) PSLSE_MEM_FAILURE;
		if (put_bytes_silent(afu->fd, 1, &buffer) != 1) {
			afu->opened = 0;
			afu->attached = 0;
		}
		return;
	}
	buffer = PSLSE_MEM_SUCCESS;
	if (put_bytes_silent(afu->fd, 1, &buffer) != 1) {
		afu->opened = 0;
		afu->attached = 0;
	}
	DPRINTF("TOUCH of addr @ 0x%016"PRIx64"\n", addr);
}

static void _handle_ack(struct cxl_afu_h *afu)
{
	uint8_t data[sizeof(uint64_t)];

	DPRINTF("MMIO ACK\n");
	if (afu->mmio.type == PSLSE_MMIO_READ64) {
		if (get_bytes_silent(afu->fd, sizeof(uint64_t), data, -1, 0)
		    < 0) {
			afu->opened = 0;
			afu->attached = 0;
			afu->mmio.data = 0xFEEDB00FFEEDB00FL;
		}
		else {
			memcpy(&(afu->mmio.data), data, sizeof(uint64_t));
			afu->mmio.data = le64toh(afu->mmio.data);
		}
	}
	if (afu->mmio.type == PSLSE_MMIO_READ32) {
		if (get_bytes_silent(afu->fd, sizeof(uint32_t), data, -1, 0)
		    < 0) {
			afu->opened = 0;
			afu->attached = 0;
			afu->mmio.data = 0xFEEDB00FL;
		}
		else {
			memcpy(&(afu->mmio.data), data, sizeof(uint32_t));
			afu->mmio.data = le32toh(afu->mmio.data);
		}
	}
	afu->mmio.state = LIBCXL_REQ_IDLE;
}

static void _handle_interrupt(struct cxl_afu_h *afu)
{
	uint16_t size, irq;
	uint8_t data[sizeof(uint16_t)];
	uint8_t type;

	DPRINTF("AFU INTERRUPT\n");
	if (get_bytes_silent(afu->fd, 4, data, -1, 0) < 0) {
		afu->opened = 0;
		afu->attached = 0;
		return;
	}
	memcpy(&irq, data, sizeof(uint16_t));
	irq = le16toh(irq);

	// Only track a single interrupt at a time
	if (afu->irq != NULL)
		return;

	size = sizeof(struct cxl_event_header) +
	       sizeof(struct cxl_event_afu_interrupt);
	afu->irq = (struct cxl_event*) calloc(1, size);
	afu->irq->header.type = CXL_EVENT_AFU_INTERRUPT;
	afu->irq->header.size = size;
	afu->irq->header.process_element = afu->context;
	afu->irq->irq.irq = irq;
	if (afu->first_event == NULL)
		afu->first_event = afu->irq;
	type = (uint8_t) CXL_EVENT_AFU_INTERRUPT;
	// FIXME: Handle errors on write?
	write(afu->pipe, &type, 1);
}

static void _req_max_int(struct cxl_afu_h *afu)
{
	uint8_t *buffer;
	int size;
	uint16_t value;

	size = 1+sizeof(uint16_t);
	buffer = (uint8_t*)malloc(size);
	buffer[0] = PSLSE_MAX_INT;
	value = htole16(afu->int_req.max);
	memcpy((char*)&(buffer[1]), (char*) &value, sizeof(uint16_t));
	if (put_bytes_silent(afu->fd, size, buffer) != size) {
		free(buffer);
		close(afu->fd);
		afu->opened = 0;
		afu->attached = 0;
		afu->int_req.max = 0;
		afu->int_req.state = LIBCXL_REQ_IDLE;
		return;
	}
	free(buffer);
	afu->int_req.state = LIBCXL_REQ_PENDING;
}

static void _pslse_attach(struct cxl_afu_h *afu)
{
	uint8_t *buffer;
	uint64_t *wed_ptr;
	int size;

	size = 1+sizeof(uint64_t);
	buffer = (uint8_t*) malloc(size);
	buffer[0] = PSLSE_ATTACH;
	wed_ptr = (uint64_t *) &(buffer[1]);
	*wed_ptr = htole64(afu->attach.wed);
	if (put_bytes_silent(afu->fd, 9, buffer) != 9) {
		free(buffer);
		close(afu->fd);
		afu->opened = 0;
		afu->attached = 0;
		afu->attach.state = LIBCXL_REQ_IDLE;
		return;
	}
	free(buffer);
	afu->attach.state = LIBCXL_REQ_PENDING;
}

static void _mmio_map(struct cxl_afu_h *afu)
{
	uint8_t *buffer;
	uint32_t *flags_ptr;
	uint32_t flags;
	int size;

	size = 1+sizeof(uint32_t);
	buffer = (uint8_t*) malloc(size);
	buffer[0] = PSLSE_MMIO_MAP;
	flags = (uint32_t) afu->mmio.data;
	flags_ptr = (uint32_t *) &(buffer[1]);
	*flags_ptr = htole32(flags);
	if (put_bytes_silent(afu->fd, size, buffer) != size) {
		free(buffer);
		close(afu->fd);
		afu->opened = 0;
		afu->attached = 0;
		afu->mmio.state = LIBCXL_REQ_IDLE;
		return;
	}
	free(buffer);
	afu->mmio.state = LIBCXL_REQ_PENDING;
}

static void _mmio_write64(struct cxl_afu_h *afu)
{
	uint8_t *buffer;
	uint64_t data;
	uint32_t addr;
	int size, offset;

	size = 1+sizeof(addr)+sizeof(data);
	buffer = (uint8_t*) malloc(size);
	buffer[0] = PSLSE_MMIO_WRITE64;
	offset = 1;
	addr = htole32(afu->mmio.addr);
	memcpy((char*)&(buffer[offset]), (char*) &addr, sizeof(addr)); 
	offset += sizeof(addr);
	data = htole64(afu->mmio.data);
	memcpy((char*)&(buffer[offset]), (char*) &data, sizeof(data));
	if (put_bytes_silent(afu->fd, size, buffer) != size) {
		free(buffer);
		close(afu->fd);
		afu->opened = 0;
		afu->attached = 0;
		afu->mmio.state = LIBCXL_REQ_IDLE;
		return;
	}
	free(buffer);
	afu->mmio.state = LIBCXL_REQ_PENDING;
}

static void _mmio_write32(struct cxl_afu_h *afu)
{
	uint8_t *buffer;
	uint32_t data;
	uint32_t addr;
	int size, offset;

	size = 1+sizeof(addr)+sizeof(data);
	buffer = (uint8_t*) malloc(size);
	buffer[0] = PSLSE_MMIO_WRITE32;
	offset = 1;
	addr = htole32(afu->mmio.addr);
	memcpy((char*)&(buffer[offset]), (char*) &addr, sizeof(addr)); 
	offset += sizeof(addr);
	data = htole32(afu->mmio.data);
	memcpy((char*)&(buffer[offset]), (char*) &data, sizeof(data));
	if (put_bytes_silent(afu->fd, size, buffer) != size) {
		free(buffer);
		close(afu->fd);
		afu->opened = 0;
		afu->attached = 0;
		afu->mmio.state = LIBCXL_REQ_IDLE;
		return;
	}
	free(buffer);
	afu->mmio.state = LIBCXL_REQ_PENDING;
}

static void _mmio_read(struct cxl_afu_h *afu)
{
	uint8_t *buffer;
	uint32_t addr;
	int size, offset;

	size = 1+sizeof(addr);
	buffer = (uint8_t*) malloc(size);
	buffer[0] = afu->mmio.type;
	offset = 1;
	addr = htole32(afu->mmio.addr);
	memcpy((char*)&(buffer[offset]), (char*) &addr, sizeof(addr));
	if (put_bytes_silent(afu->fd, size, buffer) != size) {
		free(buffer);
		close(afu->fd);
		afu->opened = 0;
		afu->attached = 0;
		afu->mmio.state = LIBCXL_REQ_IDLE;
		afu->mmio.data = 0xFEEDB00FFEEDB00FL;
		return;
	}
	free(buffer);
	afu->mmio.state = LIBCXL_REQ_PENDING;
}

static void *_psl_loop(void *ptr)
{
	struct cxl_afu_h *afu = (struct cxl_afu_h*)ptr;
	uint8_t buffer[MAX_LINE_CHARS];
	uint8_t size;
	uint64_t addr;
	uint16_t value;
	int rc;

	afu->opened = 1;
	while (afu->opened) {
		_delay_1ms();
		// Send any requests to PSLSE over socket
		if (afu->int_req.state == LIBCXL_REQ_REQUEST)
			_req_max_int(afu);
		if (afu->attach.state == LIBCXL_REQ_REQUEST)
			_pslse_attach(afu);
		if (afu->mmio.state == LIBCXL_REQ_REQUEST) {
			switch (afu->mmio.type) {
			case PSLSE_MMIO_MAP:
				_mmio_map(afu);
				break;
			case PSLSE_MMIO_WRITE64:
				_mmio_write64(afu);
				break;
			case PSLSE_MMIO_WRITE32:
				_mmio_write32(afu);
				break;
			case PSLSE_MMIO_READ64:
			case PSLSE_MMIO_READ32: /*fall through*/
				_mmio_read(afu);
				break;
			default:
				break;
			}
		}
		// Process socket input from PSLSE
		rc = bytes_ready(afu->fd, 0);
		if (rc == 0)
			continue;
		if (rc < 0) {
			afu->mapped = 0;
			afu->attached = 0;
			afu->opened = 0;
			break;
		}
		if (get_bytes_silent(afu->fd, 1, buffer, 1, 0) < 0) {
			afu->mapped = 0;
			afu->attached = 0;
			afu->opened = 0;
			break;
		}
		DPRINTF("PSL EVENT\n");
		switch (buffer[0]) {
		case PSLSE_OPEN:
			if (get_bytes_silent(afu->fd, 1, buffer, -1, 0) < 0) {
				afu->opened = 0;
				afu->open.state = LIBCXL_REQ_IDLE;
				break;
			}
			afu->context = (uint8_t) buffer[0];
			afu->open.state = LIBCXL_REQ_IDLE;
			break;
		case PSLSE_ATTACH:
			afu->attach.state = LIBCXL_REQ_IDLE;
			break;
		case PSLSE_DETACH:
			afu->attached = 0;
			break;
		case PSLSE_MAX_INT:
			size = sizeof(uint16_t);
			if (get_bytes_silent(afu->fd, size, buffer, -1, 0) < 0) 			{
				afu->opened = 0;
				break;
			}
			memcpy((char*)&value, (char*)buffer, sizeof(uint16_t));
			afu->irqs_max = le16toh(value);
			afu->int_req.state = LIBCXL_REQ_IDLE;
			break;
		case PSLSE_QUERY:
			size = sizeof(uint16_t)+sizeof(uint16_t);
			if (get_bytes_silent(afu->fd, size, buffer, -1, 0) < 0)
			{
				afu->opened = 0;
				break;
			}
			memcpy((char*) &value, (char*) &(buffer[1]), 2);
			afu->irqs_min = (long) le16toh(value);
			memcpy((char*) &value, (char*) &(buffer[3]), 2);
			afu->irqs_max = (long) le16toh(value);
			break;
		case PSLSE_MEMORY_READ:
			DPRINTF("AFU MEMORY READ\n");
			if (get_bytes_silent(afu->fd, 1, buffer, -1, 0) < 0) {
				afu->opened = 0;
				break;
			}
			size = (uint8_t) buffer[0];
			if (get_bytes_silent(afu->fd, sizeof(uint64_t), buffer,
					     -1, 0) < 0) {
				afu->opened = 0;
				break;
			}
			memcpy((char*) &addr, (char*) buffer, sizeof(uint64_t));
			addr = le64toh(addr);
			_handle_read(afu, addr, size);
			break;
		case PSLSE_MEMORY_WRITE:
			DPRINTF("AFU MEMORY WRITE\n");
			if (get_bytes_silent(afu->fd, 1, buffer, -1, 0) < 0) {
				afu->opened = 0;
				break;
			}
			size = (uint8_t) buffer[0];
			if (get_bytes_silent(afu->fd, sizeof(uint64_t), buffer,
					     -1, 0) < 0) {
				afu->opened = 0;
				break;
			}
			memcpy((char*) &addr, (char*) buffer, sizeof(uint64_t));
			addr = le64toh(addr);
			if (get_bytes_silent(afu->fd, size, buffer, -1, 0) < 0)
			{
				afu->opened = 0;
				break;
			}
			_handle_write(afu, addr, size, buffer);
			break;
		case PSLSE_MEMORY_TOUCH:
			DPRINTF("AFU MEMORY TOUCH\n");
			if (get_bytes_silent(afu->fd, 1, buffer, -1, 0) < 0) {
				afu->opened = 0;
				break;
			}
			size = buffer[0];
			if (get_bytes_silent(afu->fd, sizeof(uint64_t), buffer,
					     -1, 0) < 0) {
				afu->opened = 0;
				break;
			}
			memcpy((char*) &addr, (char*) buffer, sizeof(uint64_t));
			addr = le64toh(addr);
			_handle_touch(afu, addr, size);
			break;
		case PSLSE_MMIO_ACK:
			_handle_ack(afu);
			break;
		case PSLSE_INTERRUPT:
			_handle_interrupt(afu);
			break;
		default:
			break;
		}
	}

	afu->attached = 0;
	pthread_exit(NULL);
}

static int _pslse_connect(uint16_t *afu_map, int *fd)
{
	FILE *fp;
	uint8_t buffer[MAX_LINE_CHARS];
	struct sockaddr_in ssadr;
	struct hostent *he;
	char *host, *port_str;
	int port;

	// Get hostname and port of PSLSE server
	DPRINTF("AFU CONNECT\n");
	fp = fopen("pslse_server.dat", "r");
	if (!fp) {
		perror("fopen:pslse_server.dat");
		goto connect_fail;
	}
	if(fgets((char*) buffer, MAX_LINE_CHARS-1, fp) == NULL) {
		perror("fgets:pslse_server.dat");
		fclose(fp);
		goto connect_fail;
	}
	fclose(fp);
	host = (char*) buffer;
	port_str = strchr((char*) buffer, ':');
	*port_str = '\0';
	port_str++;
	if (!host || !port_str) {
		warn_msg("cxl_afu_open_dev:Invalid format in pslse_server.dat");
		goto connect_fail;
	}
	port = atoi(port_str);

	// Connect to PSLSE server
	if ((he = gethostbyname(host)) == NULL) {
		herror("gethostbyname");
		goto connect_fail;
	}
	memset(&ssadr, 0, sizeof(ssadr));
	memcpy(&ssadr.sin_addr, he->h_addr_list[0], he->h_length);
	ssadr.sin_family = AF_INET;
	ssadr.sin_port = htons(port);
	if ((*fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		goto connect_fail;
	}
	ssadr.sin_family = AF_INET;
	ssadr.sin_port = htons(port);
	if (connect(*fd, (struct sockaddr *)&ssadr, sizeof(ssadr)) < 0) {
		perror("connect");
		goto connect_fail;
	}
	strcpy((char*) buffer, "PSLSE");
	buffer[5] = (uint8_t) PSLSE_VERSION;
	if (put_bytes_silent(*fd, 6, buffer) != 6) {
		warn_msg("cxl_afu_open_dev:Failed to write to socket!");
		goto connect_fail;
	}
	if (get_bytes_silent(*fd, 3, buffer, -1, 0) < 0) {
		warn_msg("cxl_afu_open_dev:Socket failed open acknowledge");
		close(*fd);
		*fd = -1;
		goto connect_fail;
	}
	if (buffer[0] != (uint8_t) PSLSE_CONNECT) {
		warn_msg("cxl_afu_open_dev:PSLSE bad acknowledge");
		close(*fd);
		*fd = -1;
		goto connect_fail;
	}
	memcpy((char*) afu_map, (char*) &(buffer[1]), 2);
	*afu_map = (long) le16toh(*afu_map);
	return 0;

connect_fail:
	errno = ENODEV;
	return -1;
}

static struct cxl_adapter_h * _new_adapter(uint16_t afu_map, uint16_t position,
					   int fd)
{
	struct cxl_adapter_h *adapter;
	uint16_t mask = 0xf000;
	int id_num = 0;

	if (position == 0)
		return NULL;

	adapter = (struct cxl_adapter_h*)
		calloc(1, sizeof(struct cxl_adapter_h));
	while ((position & mask) == 0) {
		mask >>= 4;
		++id_num;
	}
	adapter->map = afu_map;
	adapter->position = position;
	adapter->mask = mask;
	adapter->fd = fd;
	adapter->id = calloc(6, sizeof(char));
	sprintf(adapter->id, "card%d", id_num);
	return adapter;
}

static struct cxl_afu_h * _new_afu(uint16_t afu_map, uint16_t position, int fd)
{
	uint8_t *buffer;
	int size;
	struct cxl_afu_h *afu;
	uint16_t adapter_mask = 0xf000;
	uint16_t afu_mask = 0x8000;
	int major = 0;
	int minor = 0;

	if (position == 0) {
		errno = ENODEV;
		return NULL;
	}
	while ((position & adapter_mask) == 0) {
		adapter_mask >>= 4;
		afu_mask >>= 4;
		++major;
	}
	while ((position & afu_mask) == 0) {
		afu_mask >>= 1;
		++minor;
	}
	afu = (struct cxl_afu_h*)
		calloc(1, sizeof(struct cxl_afu_h));
	if (afu == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	afu->fd = fd;
	afu->map = afu_map;
	afu->dbg_id = (major << 4) | minor;

	// Send PSLSE query
	size = 1+sizeof(uint8_t);
	buffer = (uint8_t*) malloc(size);
	buffer[0] = PSLSE_QUERY;
	buffer[1] = afu->dbg_id;
	if (put_bytes_silent(afu->fd, size, buffer) != size) {
		free(buffer);
		close(afu->fd);
		errno = ENODEV;
		return NULL;
	}
	free(buffer);

	afu->adapter = major;
	afu->position = position;
	afu->id = calloc(7, sizeof(char));
	afu->int_req.state = LIBCXL_REQ_IDLE;
	afu->open.state = LIBCXL_REQ_IDLE;
	afu->attach.state = LIBCXL_REQ_IDLE;
	afu->mmio.state = LIBCXL_REQ_IDLE;
	sprintf(afu->id, "afu%d.%d", major, minor);

	return afu;
}

static void _release_afus(struct cxl_afu_h *afu)
{
	struct cxl_afu_h *last, *current;
	uint8_t rc = PSLSE_DETACH;
	int adapter;

	if (afu==NULL)
		return;

	current = afu->_head;
	while (current->adapter < afu->adapter) {
		last = current;
		current = current->_next;
	}
	adapter = afu->adapter;
	current = afu;
	while ((current != NULL) && (current->adapter == adapter)) {
		afu = current;
		current = current->_next;
		if (afu->fd) {
			put_bytes_silent(afu->fd, 1, &rc);
			close(afu->fd);
		}
		if (afu->id)
			free(afu->id);
		free(afu);
	}
}

static void _release_adapters(struct cxl_adapter_h *adapter)
{
	struct cxl_adapter_h *current;
	uint8_t rc = PSLSE_DETACH;

	_release_afus(adapter->afu_list);
	current = adapter;
	while (current != NULL) {
		adapter = current;
		current = current->_next;
		// Disconnect from PSLSE
		if (adapter->fd) {
			put_bytes_silent(adapter->fd, 1, &rc);
			close(adapter->fd);
		}
		free(adapter->id);
		free(adapter);
	}
}

static struct cxl_afu_h * _pslse_open(int fd, uint16_t afu_map, uint8_t major,
				      uint8_t minor, char afu_type)
{
	struct cxl_afu_h *afu;
	uint8_t *buffer;
	uint16_t position;

	position = 0x8000;
	position >>= 4*major;
	position >>= minor;
	if ((afu_map & position) != position) {
		warn_msg("open:AFU not in system");
		close(fd);
		errno = ENODEV;
		return NULL;
	}

	// Create struct for AFU
	afu = _new_afu(afu_map, position, fd);
	if (afu == NULL)
		return NULL;

	buffer = (uint8_t*) calloc(1, MAX_LINE_CHARS);
	buffer[0] = (uint8_t) PSLSE_OPEN;
	buffer[1] = afu->dbg_id;
	buffer[2] = afu_type;
	afu->fd = fd;
	if (put_bytes_silent(afu->fd, 3, buffer) != 3) {
		warn_msg("open:Failed to write to socket");
		free(buffer);
		goto open_fail;
	}
	free(buffer);

	afu->_head = afu;
	afu->adapter = major;
	afu->id = (char *) malloc(7);
	afu->open.state = LIBCXL_REQ_PENDING;

	// Start thread
	if (pthread_create(&(afu->thread), NULL, _psl_loop, afu)) {
		perror("pthread_create");
		close(afu->fd);
		goto open_fail;
	}

	// Wait for open acknowledgement
	while (afu->open.state != LIBCXL_REQ_IDLE) /*infinite loop*/
		_delay_1ms();

	if (!afu->opened) {
		pthread_join(afu->thread, NULL);
		goto open_fail;
	}

	sprintf(afu->id, "afu%d.%d", major, minor);

	return afu;

open_fail:
	free(afu);
	errno = ENODEV;
	return NULL;
}

struct cxl_adapter_h * cxl_adapter_next(struct cxl_adapter_h *adapter)
{
	struct cxl_adapter_h *head;
	uint16_t afu_map, afu_mask;
	int fd;

	// First adapter
	if (adapter == NULL) {
		// Query PSLSE
		if (_pslse_connect(&afu_map, &fd) < 0)
			return NULL;
		// No devices?
		assert (afu_map != 0);
		afu_mask = 0x8000;
		// Find first AFU and return struct for it
		while ((afu_map & afu_mask) != afu_mask)
			afu_mask >>= 1;
		head = _new_adapter(afu_map, afu_mask, fd);
		head->_head = head;
		return head;
	}

	// Return next adapter if already set
	if (adapter->_next != NULL) {
		adapter->_next->fd = adapter->fd;
		adapter->fd = 0;
		return adapter->_next;
	}

	// Find next adapter
	afu_mask = adapter->position;
	afu_map = adapter->map;
	while (((afu_mask & ~adapter->mask) == 0) && (afu_mask != 0))
		afu_mask >>= 1;

	// Find first AFU on another adapter
	while (((afu_map & afu_mask) != afu_mask) && (afu_mask != 0))
		afu_mask >>= 1;

	// No more AFUs
	if (afu_mask == 0) {
		_release_adapters(adapter->_head);
		return NULL;
	}

	// Update pointers and return next adapter
	adapter->_next = _new_adapter(afu_map, afu_mask, adapter->fd);
	adapter->_next->_head = adapter->_head;
	adapter->fd = 0;
	return adapter->_next;
}

char * cxl_adapter_dev_name(struct cxl_adapter_h *adapter)
{
	if (adapter==NULL)
		return NULL;

	return adapter->id;
}

void cxl_adapter_free(struct cxl_adapter_h *adapter)
{
	struct cxl_adapter_h *head, *current;

	if (adapter==NULL)
		return;

	// If removing head then update all head pointers to next
	current = head = adapter->_head;
	while ((head == adapter) && (current != NULL)) {
		current->_head = head->_next;
		current = current->_next;
	}

	// Update list to skip adapter being removed
	current = adapter->_head;
	while (current != NULL) {
		if (current->_next == adapter)
			current->_next = adapter->_next;
		current = current->_next;
	}

	// Free memory for adapter
	_release_afus(adapter->afu_list);
	if (adapter->id)
		free(adapter->id);
	free(adapter);
}

struct cxl_afu_h * cxl_adapter_afu_next(struct cxl_adapter_h *adapter, struct cxl_afu_h *afu)
{
	struct cxl_afu_h *head;
	uint16_t afu_map, afu_mask;
	int fd;

	if (adapter==NULL)
		return NULL;

	afu_mask = adapter->position;

	// Query PSLSE
	if (adapter->fd==0) {
		if (_pslse_connect(&afu_map, &fd) < 0)
			return NULL;
	}
	else {
		afu_map = adapter->map;
	}

	// First afu
	if (afu == NULL) {
		// No devices?
		assert (afu_map != 0);
		// Find first AFU and return struct for it
		afu_mask = adapter->mask & 0x8888;
		while ((afu_map & afu_mask) != afu_mask)
			afu_mask >>= 1;
		head = _new_afu(afu_map, afu_mask, adapter->fd);
		adapter->fd = 0;
		head->_head = head;
		if (head != NULL)
			head->_head = head;
		return head;
	}

	// Return next afu if already set
	if (afu->_next != NULL) {
		afu->_next->fd = afu->fd;
		afu->fd = 0;
		return afu->_next;
	}

	// Find next afu on this adapter
	afu_mask = afu->position >> 1;
	while (((afu_mask & adapter->mask)!=0) && ((afu_mask & afu->map)==0))
		afu_mask >>= 1;

	// No more AFUs on this adapter
	if ((afu_mask & adapter->mask)==0) {
		_release_afus(adapter->afu_list);
		return NULL;
	}

	// Update pointers and return next afu
	afu->_next = _new_afu(afu_map, afu_mask, afu->fd);
	afu->_next->_head = afu->_head;
	afu->fd = 0;
	return afu->_next;
}

struct cxl_afu_h * cxl_afu_next(struct cxl_afu_h *afu)
{
	struct cxl_afu_h *head;
	uint16_t afu_map, afu_mask;
	int fd;

	if ((afu==NULL) || (afu->fd==0)) {
		// Query PSLSE
		if (_pslse_connect(&afu_map, &fd) < 0)
			return NULL;
	}
	else {
		afu_map = afu->map;
	}

	// First afu
	if (afu == NULL) {
		// No devices?
		assert (afu_map != 0);
		afu_mask = 0x8000;
		// Find first AFU and return struct for it
		while ((afu_map & afu_mask) != afu_mask)
			afu_mask >>= 1;
		head = _new_afu(afu_map, afu_mask, fd);
		head->_head = head;
		return head;
	}

	// Return next afu if already set
	if (afu->_next != NULL) {
		afu->_next->fd = afu->fd;
		afu->fd = 0;
		return afu->_next;
	}

	// Find next afu
	afu_mask = afu->position;
	afu_mask >>= 1;
	while ((afu_mask != 0) && ((afu_mask & afu->map) == 0))
		afu_mask >>= 1;

	// No more AFUs
	if (afu_mask == 0) {
		_release_afus(afu->_head);
		return NULL;
	}

	// Update pointers and return next afu
	afu->_next = _new_afu(afu_map, afu_mask, afu->fd);
	afu->_next->_head = afu->_head;
	afu->fd = 0;
	return afu->_next;
}

char * cxl_afu_dev_name(struct cxl_afu_h *afu)
{
	return afu->id;
}

struct cxl_afu_h * cxl_afu_open_dev(char *path)
{
	uint16_t afu_map;
	uint8_t major, minor;
	char *afu_id;
	char afu_type;
	int fd;

	if (_pslse_connect(&afu_map, &fd) < 0)
		return NULL;

	// Discover AFU position
	afu_id = strrchr(path, '/');
	afu_id++;
	if ((afu_id[3] < '0') || (afu_id[3] > '3')) {
		warn_msg("Invalid afu major: %c", afu_id[3]);
		errno = ENODEV;
		return NULL;
	}
	if ((afu_id[5] < '0') || (afu_id[5] > '3')) {
		warn_msg("Invalid afu minor: %c", afu_id[5]);
		errno = ENODEV;
		return NULL;
	}
	major = afu_id[3] - '0';
	minor = afu_id[5] - '0';
	afu_type = afu_id[6];

	return _pslse_open(fd, afu_map, major, minor, afu_type);
}

struct cxl_afu_h * cxl_afu_open_h(struct cxl_afu_h *afu, enum cxl_views view)
{
	uint8_t major, minor;
	uint16_t mask;
	char afu_type;

	if (afu==NULL) {
		errno = ENODEV;
		return NULL;
	}

	// Query PSLSE
	if (afu->fd==0) {
		if (_pslse_connect(&(afu->map), &afu->fd) < 0)
			return NULL;
	}

	mask = 0xf000;
	major = minor = 0;
	while (((mask & afu->position) != afu->position) && (mask != 0)) {
		mask >>= 4;
		major++;
	}
	mask &= 0x8888;
	while (((mask & afu->position) != afu->position) && (mask != 0)) {
		mask >>= 1;
		minor++;
	}
	switch (view) {
	case CXL_VIEW_DEDICATED:
		afu_type = 'd';
		break;
	case CXL_VIEW_MASTER:
		afu_type = 'm';
		break;
	case CXL_VIEW_SLAVE:
		afu_type = 's';
		break;
	default:
		errno = ENODEV;
		return NULL;
	}
	return _pslse_open(afu->fd, afu->map, major, minor, afu_type);
}

void cxl_afu_free(struct cxl_afu_h *afu) {
	uint8_t buffer;
	int rc;

	if (!afu->opened)
		goto free_done;

	DPRINTF("AFU FREE\n");
	buffer = PSLSE_DETACH;
	rc = put_bytes_silent(afu->fd, 1, &buffer);
	if (rc==1) {
		while (afu->attached) /*infinite loop*/
			_delay_1ms();
	}
	close(afu->fd);
	afu->opened = 0;

free_done:
	if (afu->id != NULL)
		free(afu->id);
	pthread_join(afu->thread, NULL);
	free(afu);
}

int cxl_afu_opened(struct cxl_afu_h *afu) {
	return afu->opened;
}

int cxl_afu_attach_full(struct cxl_afu_h *afu, __u64 wed, __u16 num_interrupts,
			__u64 amr) {
	// Request maximum interrupts
	afu->int_req.max = num_interrupts;
	afu->int_req.state = LIBCXL_REQ_REQUEST;
	while (afu->int_req.state != LIBCXL_REQ_IDLE) /*infinite loop*/
		_delay_1ms();

	return cxl_afu_attach(afu, wed);
}

int cxl_afu_attach(struct cxl_afu_h *afu, __u64 wed) {
	DPRINTF("AFU ATTACH\n");
	if (!afu->opened) {
		warn_msg("cxl_afu_attach: Must open AFU first");
		errno = ENODEV;
		return -1;
	}

	if (afu->attached) {
		warn_msg("cxl_afu_attach: AFU already attached");
		errno = ENODEV;
		return -1;
	}

	// Perform PSLSE attach
	afu->attach.state = LIBCXL_REQ_REQUEST;
	afu->attach.wed = wed;
	while (afu->attach.state != LIBCXL_REQ_IDLE) /*infinite loop*/
		_delay_1ms();
	afu->attached = 1;

	return 0;
}

int cxl_afu_fd(struct cxl_afu_h *afu)
{
	int fd[2];

	pipe(fd);
	afu->pipe = fd[0];
	return fd[1];
}

int cxl_get_api_version(struct cxl_afu_h *afu, long *valp)
{
	if ((afu==NULL) || (afu->opened))
		return -1;
	*valp = API_VERSION;
	return 0;
}

int cxl_get_api_version_compatible(struct cxl_afu_h *afu, long *valp)
{
	if ((afu==NULL) || (afu->opened))
		return -1;
	*valp = API_VERSION_COMPATIBLE;
	return 0;
}

int cxl_get_irqs_max(struct cxl_afu_h *afu, long *valp)
{
	if ((afu==NULL) || (afu->opened))
		return -1;
	*valp = afu->irqs_max;
	return 0;
}

int cxl_get_irqs_min(struct cxl_afu_h *afu, long *valp)
{
	if ((afu==NULL) || (afu->opened))
		return -1;
	*valp = afu->irqs_min;
	return 0;
}

int cxl_event_pending(struct cxl_afu_h *afu)
{
	if (afu->first_event == NULL)
		return 0;
	return 1;
}

int cxl_read_event(struct cxl_afu_h *afu, struct cxl_event *event)
{
	if (afu == NULL || event == NULL) {
		errno = EINVAL;
		return -1;
	}

	// Function will block until event occurs
	while (afu->opened && (afu->first_event == NULL)) /*infinite loop*/
		_delay_1ms();

	// Copy event data, free and move next event, if any, to first
	memcpy(event, afu->first_event, afu->first_event->header.size);
	if (afu->first_event == afu->irq) {
		free(afu->irq);
		afu->first_event = afu->dsi;
	}
	else if (afu->first_event == afu->dsi) {
		free(afu->dsi);
		afu->first_event = afu->irq;
	}
	return 0;
}

int cxl_read_expected_event(struct cxl_afu_h *afu, struct cxl_event *event,
			    __u32 type, __u16 irq) {
	if (cxl_read_event(afu, event) < 0)
		return -1;

	if (event->header.type != type)
		return -1;

	if ((event->header.type == CXL_EVENT_AFU_INTERRUPT) &&
	    (event->irq.irq != irq))
		return -1;

	return 0;
}

int cxl_mmio_map(struct cxl_afu_h *afu, __u32 flags)
{
	DPRINTF("MMIO MAP\n");
	if (!afu->opened) {
		goto map_fail;
	}

	if (!afu->attached) {
		printf("cxl_mmio_map: Must attach first!\n");
		goto map_fail;
	}

	if (flags & ~(CXL_MMIO_FLAGS)) {
		printf("cxl_mmio_map: Invalid flags!\n");
		goto map_fail;
	}

	// Send MMIO map to PSLSE
	afu->mmio.type = PSLSE_MMIO_MAP;
	afu->mmio.data = (uint64_t) flags;
	afu->mmio.state = LIBCXL_REQ_REQUEST;
	while (afu->mmio.state != LIBCXL_REQ_IDLE) /*infinite loop*/
		_delay_1ms();
	afu->mapped = 1;

	return 0;
map_fail:
	errno = ENODEV;
	return -1;
}

int cxl_mmio_unmap(struct cxl_afu_h *afu)
{
	afu->mapped = 0;
	return 0;
}

int cxl_mmio_write64(struct cxl_afu_h *afu, uint64_t offset, uint64_t data)
{
	if (offset & 0x7) {
		errno = EINVAL;
		return -1;
	}
	if ((afu == NULL) || !afu->mapped)
		goto write64_fail;

	// Send MMIO map to PSLSE
	afu->mmio.type = PSLSE_MMIO_WRITE64;
	afu->mmio.addr = (uint32_t) offset;
	afu->mmio.data = data;
	afu->mmio.state = LIBCXL_REQ_REQUEST;
	while (afu->mmio.state != LIBCXL_REQ_IDLE) /*infinite loop*/
		_delay_1ms();

	if (!afu->opened)
		goto write64_fail;

	return 0;

write64_fail:
	errno = ENODEV;
	return -1;
}

int cxl_mmio_read64(struct cxl_afu_h *afu, uint64_t offset, uint64_t * data)
{
	if (offset & 0x7) {
		errno = EINVAL;
		return -1;
	}
	if ((afu == NULL) || !afu->mapped)
		goto read64_fail;

	// Send MMIO map to PSLSE
	afu->mmio.type = PSLSE_MMIO_READ64;
	afu->mmio.addr = (uint32_t) offset;
	afu->mmio.state = LIBCXL_REQ_REQUEST;
	while (afu->mmio.state != LIBCXL_REQ_IDLE) /*infinite loop*/
		_delay_1ms();
	*data = afu->mmio.data;

	if (!afu->opened)
		goto read64_fail;

	return 0;

read64_fail:
	errno = ENODEV;
	return -1;
}


int cxl_mmio_write32(struct cxl_afu_h *afu, uint64_t offset, uint32_t data)
{
	if (offset & 0x7) {
		errno = EINVAL;
		return -1;
	}
	if ((afu == NULL) || !afu->mapped)
		goto write32_fail;

	// Send MMIO map to PSLSE
	afu->mmio.type = PSLSE_MMIO_WRITE32;
	afu->mmio.addr = (uint32_t) offset;
	afu->mmio.data = (uint64_t) data;
	afu->mmio.state = LIBCXL_REQ_REQUEST;
	while (afu->mmio.state != LIBCXL_REQ_IDLE) /*infinite loop*/
		_delay_1ms();

	if (!afu->opened)
		goto write32_fail;

	return 0;

write32_fail:
	errno = ENODEV;
	return -1;
}

int cxl_mmio_read32(struct cxl_afu_h *afu, uint64_t offset, uint32_t * data)
{
	if (offset & 0x7) {
		errno = EINVAL;
		return -1;
	}
	if ((afu == NULL) || !afu->mapped)
		goto read32_fail;

	// Send MMIO map to PSLSE
	afu->mmio.type = PSLSE_MMIO_READ32;
	afu->mmio.addr = (uint32_t) offset;
	afu->mmio.state = LIBCXL_REQ_REQUEST;
	while (afu->mmio.state != LIBCXL_REQ_IDLE) /*infinite loop*/
		_delay_1ms();
	*data = (uint32_t) afu->mmio.data;

	if (!afu->opened)
		goto read32_fail;

	return 0;

read32_fail:
	errno = ENODEV;
	return -1;
}

