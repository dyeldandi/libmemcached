/*  vim:expandtab:shiftwidth=2:tabstop=2:smarttab:
 * 
 *  Libmemcached library
 *
 *  Copyright (C) 2011 Data Differential, http://datadifferential.com/
 *  Copyright (C) 2006-2009 Brian Aker All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *      * Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *  copyright notice, this list of conditions and the following disclaimer
 *  in the documentation and/or other materials provided with the
 *  distribution.
 *
 *      * The names of its contributors may not be used to endorse or
 *  promote products derived from this software without specific prior
 *  written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <libmemcached/common.h>

enum memcached_storage_action_t {
  SET_OP,
  REPLACE_OP,
  ADD_OP,
  PREPEND_OP,
  APPEND_OP,
  CAS_OP
};

/* Inline this */
static inline const char *storage_op_string(memcached_storage_action_t verb)
{
  switch (verb)
  {
  case REPLACE_OP:
    return "replace ";

  case ADD_OP:
    return "add ";

  case PREPEND_OP:
    return "prepend ";

  case APPEND_OP:
    return "append ";

  case CAS_OP:
    return "cas ";

  case SET_OP:
    break;
  }

  return "set ";
}

static inline uint8_t get_com_code(const memcached_storage_action_t verb, const bool reply)
{
  if (reply == false)
  {
    switch (verb)
    {
    case SET_OP:
      return PROTOCOL_BINARY_CMD_SETQ;

    case ADD_OP:
      return PROTOCOL_BINARY_CMD_ADDQ;

    case CAS_OP: /* FALLTHROUGH */
    case REPLACE_OP:
      return PROTOCOL_BINARY_CMD_REPLACEQ;

    case APPEND_OP:
      return PROTOCOL_BINARY_CMD_APPENDQ;

    case PREPEND_OP:
      return PROTOCOL_BINARY_CMD_PREPENDQ;
    }
  }

  switch (verb)
  {
  case SET_OP:
    break;

  case ADD_OP:
    return PROTOCOL_BINARY_CMD_ADD;

  case CAS_OP: /* FALLTHROUGH */
  case REPLACE_OP:
    return PROTOCOL_BINARY_CMD_REPLACE;

  case APPEND_OP:
    return PROTOCOL_BINARY_CMD_APPEND;

  case PREPEND_OP:
    return PROTOCOL_BINARY_CMD_PREPEND;
  }

  return PROTOCOL_BINARY_CMD_SET;
}

static memcached_return_t memcached_send_binary(memcached_st *ptr,
                                                memcached_server_write_instance_st server,
                                                uint32_t server_key,
                                                const char *key,
                                                const size_t key_length,
                                                const char *value,
                                                const size_t value_length,
                                                const time_t expiration,
                                                const uint32_t flags,
                                                const uint64_t cas,
                                                const bool flush,
                                                const bool reply,
                                                memcached_storage_action_t verb)
{
  protocol_binary_request_set request= {};
  size_t send_length= sizeof(request.bytes);

  request.message.header.request.magic= PROTOCOL_BINARY_REQ;
  request.message.header.request.opcode= get_com_code(verb, reply);
  request.message.header.request.keylen= htons((uint16_t)(key_length + memcached_array_size(ptr->_namespace)));
  request.message.header.request.datatype= PROTOCOL_BINARY_RAW_BYTES;
  if (verb == APPEND_OP or verb == PREPEND_OP)
  {
    send_length -= 8; /* append & prepend does not contain extras! */
  }
  else
  {
    request.message.header.request.extlen= 8;
    request.message.body.flags= htonl(flags);
    request.message.body.expiration= htonl((uint32_t)expiration);
  }

  request.message.header.request.bodylen= htonl((uint32_t) (key_length + memcached_array_size(ptr->_namespace) + value_length +
                                                            request.message.header.request.extlen));

  if (cas)
  {
    request.message.header.request.cas= memcached_htonll(cas);
  }

  struct libmemcached_io_vector_st vector[]=
  {
    { request.bytes, send_length },
    { memcached_array_string(ptr->_namespace),  memcached_array_size(ptr->_namespace) },
    { key, key_length },
    { value, value_length }
  };

  /* write the header */
  memcached_return_t rc;
  if ((rc= memcached_vdo(server, vector, 4, flush)) != MEMCACHED_SUCCESS)
  {
    memcached_io_reset(server);

    if (memcached_has_error(ptr))
    {
      memcached_set_error(*server, rc, MEMCACHED_AT);
    }

    return MEMCACHED_WRITE_FAILURE;
  }

  if (verb == SET_OP and ptr->number_of_replicas > 0)
  {
    request.message.header.request.opcode= PROTOCOL_BINARY_CMD_SETQ;
    WATCHPOINT_STRING("replicating");

    for (uint32_t x= 0; x < ptr->number_of_replicas; x++)
    {
      ++server_key;
      if (server_key == memcached_server_count(ptr))
      {
        server_key= 0;
      }

      memcached_server_write_instance_st instance= memcached_server_instance_fetch(ptr, server_key);

      if (memcached_vdo(instance, vector, 4, false) != MEMCACHED_SUCCESS)
      {
        memcached_io_reset(instance);
      }
      else
      {
        memcached_server_response_decrement(instance);
      }
    }
  }

  if (flush == false)
  {
    return MEMCACHED_BUFFERED;
  }

  // No reply always assumes success
  if (reply == false)
  {
    return MEMCACHED_SUCCESS;
  }

  return memcached_response(server, NULL, 0, NULL);
}

static memcached_return_t memcached_send_ascii(memcached_st *ptr,
                                               memcached_server_write_instance_st instance,
                                               const char *key,
                                               const size_t key_length,
                                               const char *value,
                                               const size_t value_length,
                                               const time_t expiration,
                                               const uint32_t flags,
                                               const uint64_t cas,
                                               const bool flush,
                                               const bool reply,
                                               const memcached_storage_action_t verb)
{
  char flags_buffer[MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH +1];
  int flags_buffer_length= snprintf(flags_buffer, sizeof(flags_buffer), " %u", flags);
  if (size_t(flags_buffer_length) >= sizeof(flags_buffer) or flags_buffer_length < 0)
  {
    return memcached_set_error(*instance, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT, 
                               memcached_literal_param("snprintf(MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH)"));
  }

  char expiration_buffer[MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH +1];
  int expiration_buffer_length= snprintf(expiration_buffer, sizeof(expiration_buffer), " %llu", (unsigned long long)expiration);
  if (size_t(expiration_buffer_length) >= sizeof(expiration_buffer) or expiration_buffer_length < 0)
  {
    return memcached_set_error(*instance, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT, 
                               memcached_literal_param("snprintf(MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH)"));
  }

  char value_buffer[MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH +1];
  int value_buffer_length= snprintf(value_buffer, sizeof(value_buffer), " %llu", (unsigned long long)value_length);
  if (size_t(value_buffer_length) >= sizeof(value_buffer) or value_buffer_length < 0)
  {
    return memcached_set_error(*instance, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT, 
                               memcached_literal_param("snprintf(MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH)"));
  }

  char cas_buffer[MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH +1];
  int cas_buffer_length= 0;
  if (cas)
  {
    cas_buffer_length= snprintf(cas_buffer, sizeof(cas_buffer), " %llu", (unsigned long long)cas);
    if (size_t(cas_buffer_length) >= sizeof(cas_buffer) or cas_buffer_length < 0)
    {
      return memcached_set_error(*instance, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT, 
                                 memcached_literal_param("snprintf(MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH)"));
    }
  }

  struct libmemcached_io_vector_st vector[]=
  {
    { storage_op_string(verb), strlen(storage_op_string(verb))},
    { memcached_array_string(ptr->_namespace), memcached_array_size(ptr->_namespace) },
    { key, key_length },
    { flags_buffer, flags_buffer_length },
    { expiration_buffer, expiration_buffer_length },
    { value_buffer, value_buffer_length },
    { cas_buffer, cas_buffer_length },
    { " noreply", reply ? 0 : memcached_literal_param_size(" noreply") },
    { memcached_literal_param("\r\n") },
    { value, value_length },
    { memcached_literal_param("\r\n") }
  };

  /* Send command header */
  memcached_return_t rc=  memcached_vdo(instance, vector, 11, flush);
  if (rc == MEMCACHED_SUCCESS)
  {
    if (flush == false)
    {
      return MEMCACHED_BUFFERED;
    }

    if (reply == false)
    {
      return MEMCACHED_SUCCESS;
    }

    char buffer[MEMCACHED_DEFAULT_COMMAND_SIZE];
    rc= memcached_response(instance, buffer, MEMCACHED_DEFAULT_COMMAND_SIZE, NULL);

    if (rc == MEMCACHED_STORED)
    {
      return MEMCACHED_SUCCESS;
    }
  }

  if (rc == MEMCACHED_WRITE_FAILURE)
  {
    memcached_io_reset(instance);
  }

  assert(memcached_failed(rc));
  if (memcached_has_error(ptr) == false)
  {
    return memcached_set_error(*ptr, rc, MEMCACHED_AT);
  }

  return rc;
}

static inline memcached_return_t memcached_send(memcached_st *ptr,
                                                const char *group_key, size_t group_key_length,
                                                const char *key, size_t key_length,
                                                const char *value, size_t value_length,
                                                const time_t expiration,
                                                const uint32_t flags,
                                                const uint64_t cas,
                                                memcached_storage_action_t verb)
{
  memcached_return_t rc;
  if (memcached_failed(rc= initialize_query(ptr, true)))
  {
    return rc;
  }

  if (memcached_failed(rc= memcached_validate_key_length(key_length, memcached_is_binary(ptr))))
  {
    return rc;
  }

  if (memcached_failed(memcached_key_test(*ptr, (const char **)&key, &key_length, 1)))
  {
    return MEMCACHED_BAD_KEY_PROVIDED;
  }

  uint32_t server_key= memcached_generate_hash_with_redistribution(ptr, group_key, group_key_length);
  memcached_server_write_instance_st instance= memcached_server_instance_fetch(ptr, server_key);

  WATCHPOINT_SET(instance->io_wait_count.read= 0);
  WATCHPOINT_SET(instance->io_wait_count.write= 0);


  bool flush= true;
  if (memcached_is_buffering(instance->root) and verb == SET_OP)
  {
    flush= false;
  }

  bool reply= memcached_is_replying(ptr);

  if (memcached_is_binary(ptr))
  {
    return memcached_send_binary(ptr, instance, server_key,
                                 key, key_length,
                                 value, value_length, expiration,
                                 flags, cas, flush, reply, verb);
  }

  return memcached_send_ascii(ptr, instance,
                              key, key_length,
                              value, value_length, expiration,
                              flags, cas, flush, reply, verb);
}


memcached_return_t memcached_set(memcached_st *ptr, const char *key, size_t key_length,
                                 const char *value, size_t value_length,
                                 time_t expiration,
                                 uint32_t flags)
{
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_SET_START();
  rc= memcached_send(ptr, key, key_length,
                     key, key_length, value, value_length,
                     expiration, flags, 0, SET_OP);
  LIBMEMCACHED_MEMCACHED_SET_END();
  return rc;
}

memcached_return_t memcached_add(memcached_st *ptr,
                                 const char *key, size_t key_length,
                                 const char *value, size_t value_length,
                                 time_t expiration,
                                 uint32_t flags)
{
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_ADD_START();
  rc= memcached_send(ptr, key, key_length,
                     key, key_length, value, value_length,
                     expiration, flags, 0, ADD_OP);

  if (rc == MEMCACHED_NOTSTORED or rc == MEMCACHED_DATA_EXISTS)
  {
    memcached_set_error(*ptr, rc, MEMCACHED_AT);
  }
  LIBMEMCACHED_MEMCACHED_ADD_END();
  return rc;
}

memcached_return_t memcached_replace(memcached_st *ptr,
                                     const char *key, size_t key_length,
                                     const char *value, size_t value_length,
                                     time_t expiration,
                                     uint32_t flags)
{
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_REPLACE_START();
  rc= memcached_send(ptr, key, key_length,
                     key, key_length, value, value_length,
                     expiration, flags, 0, REPLACE_OP);
  LIBMEMCACHED_MEMCACHED_REPLACE_END();
  return rc;
}

memcached_return_t memcached_prepend(memcached_st *ptr,
                                     const char *key, size_t key_length,
                                     const char *value, size_t value_length,
                                     time_t expiration,
                                     uint32_t flags)
{
  memcached_return_t rc;
  rc= memcached_send(ptr, key, key_length,
                     key, key_length, value, value_length,
                     expiration, flags, 0, PREPEND_OP);
  return rc;
}

memcached_return_t memcached_append(memcached_st *ptr,
                                    const char *key, size_t key_length,
                                    const char *value, size_t value_length,
                                    time_t expiration,
                                    uint32_t flags)
{
  memcached_return_t rc;
  rc= memcached_send(ptr, key, key_length,
                     key, key_length, value, value_length,
                     expiration, flags, 0, APPEND_OP);
  return rc;
}

memcached_return_t memcached_cas(memcached_st *ptr,
                                 const char *key, size_t key_length,
                                 const char *value, size_t value_length,
                                 time_t expiration,
                                 uint32_t flags,
                                 uint64_t cas)
{
  memcached_return_t rc;
  rc= memcached_send(ptr, key, key_length,
                     key, key_length, value, value_length,
                     expiration, flags, cas, CAS_OP);
  return rc;
}

memcached_return_t memcached_set_by_key(memcached_st *ptr,
                                        const char *group_key,
                                        size_t group_key_length,
                                        const char *key, size_t key_length,
                                        const char *value, size_t value_length,
                                        time_t expiration,
                                        uint32_t flags)
{
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_SET_START();
  rc= memcached_send(ptr, group_key, group_key_length,
                     key, key_length, value, value_length,
                     expiration, flags, 0, SET_OP);
  LIBMEMCACHED_MEMCACHED_SET_END();
  return rc;
}

memcached_return_t memcached_add_by_key(memcached_st *ptr,
                                        const char *group_key, size_t group_key_length,
                                        const char *key, size_t key_length,
                                        const char *value, size_t value_length,
                                        time_t expiration,
                                        uint32_t flags)
{
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_ADD_START();
  rc= memcached_send(ptr, group_key, group_key_length,
                     key, key_length, value, value_length,
                     expiration, flags, 0, ADD_OP);
  LIBMEMCACHED_MEMCACHED_ADD_END();
  return rc;
}

memcached_return_t memcached_replace_by_key(memcached_st *ptr,
                                            const char *group_key, size_t group_key_length,
                                            const char *key, size_t key_length,
                                            const char *value, size_t value_length,
                                            time_t expiration,
                                            uint32_t flags)
{
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_REPLACE_START();
  rc= memcached_send(ptr, group_key, group_key_length,
                     key, key_length, value, value_length,
                     expiration, flags, 0, REPLACE_OP);
  LIBMEMCACHED_MEMCACHED_REPLACE_END();
  return rc;
}

memcached_return_t memcached_prepend_by_key(memcached_st *ptr,
                                            const char *group_key, size_t group_key_length,
                                            const char *key, size_t key_length,
                                            const char *value, size_t value_length,
                                            time_t expiration,
                                            uint32_t flags)
{
  return memcached_send(ptr, group_key, group_key_length,
                        key, key_length, value, value_length,
                        expiration, flags, 0, PREPEND_OP);
}

memcached_return_t memcached_append_by_key(memcached_st *ptr,
                                           const char *group_key, size_t group_key_length,
                                           const char *key, size_t key_length,
                                           const char *value, size_t value_length,
                                           time_t expiration,
                                           uint32_t flags)
{
  return memcached_send(ptr, group_key, group_key_length,
                        key, key_length, value, value_length,
                        expiration, flags, 0, APPEND_OP);
}

memcached_return_t memcached_cas_by_key(memcached_st *ptr,
                                        const char *group_key, size_t group_key_length,
                                        const char *key, size_t key_length,
                                        const char *value, size_t value_length,
                                        time_t expiration,
                                        uint32_t flags,
                                        uint64_t cas)
{
  return  memcached_send(ptr, group_key, group_key_length,
                         key, key_length, value, value_length,
                         expiration, flags, cas, CAS_OP);
}

