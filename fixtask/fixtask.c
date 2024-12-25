/*
 * FixScript Task v0.6 - https://www.fixscript.org/
 * Copyright (c) 2020-2024 Martin Dvorak <jezek2@advel.cz>
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose, 
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#ifdef __wasm__
#include <wasm-support.h>
#else
#include <sys/time.h>
#endif
#endif
#include "fixtask.h"

enum {
   HANDLE_destroy,
   HANDLE_compare,
   HANDLE_calc_hash,
   HANDLE_to_string,
   HANDLE_mark_refs,
   HANDLE_SIZE
};

enum {
   CHANNEL_OWNED    = 0,
   CHANNEL_SENDER   = 1,
   CHANNEL_RECEIVER = 2,
   CHANNEL_BOTH     = 3
};

enum {
   CHECK_ARRAY,
   CHECK_STRING,
   CHECK_HASH,
   CHECK_SHARED,
   CHECK_FUNCREF,
   CHECK_WEAKREF,
   CHECK_HANDLE
};

#ifdef _WIN32
#define ETIMEDOUT -1000
typedef CRITICAL_SECTION pthread_mutex_t;
typedef HANDLE pthread_cond_t;
#endif

typedef struct {
   HeapCreateFunc create_func;
   void *create_data;
   LoadScriptFunc load_func;
   void *load_data;
} HeapCreateData;

#ifdef __wasm__
typedef struct TaskSender {
   void *task;
   Heap *heap;
   Value arr, msg;
   ContinuationFunc wake_func;
   ContinuationResultFunc cont_func;
   void *cont_data;
   struct TaskSender *next;
} TaskSender;

typedef struct TaskReceiver {
   void *task;
   Heap *heap;
   Value arr;
   ContinuationFunc wake_func;
   ContinuationResultFunc cont_func;
   void *cont_data;
   wasm_timer_t cancel_timer;
   struct TaskReceiver *next;
} TaskReceiver;
#endif

typedef struct {
   volatile int refcnt;
   HeapCreateData hc;
   int load_scripts;
   char *fname, *func_name;
   Heap *comm_heap;
   Value comm_arr, reply_arr;
   int max_messages;
   Value start_params;
   Value task_val;
   pthread_mutex_t mutex;
   pthread_cond_t cond;
#ifdef __wasm__
   TaskSender *wasm_senders;
   TaskReceiver *wasm_receivers;
#endif
} Task;

typedef struct ComputeHeap {
   Heap *heap;
   Value process_func;
   Value process_data;
   Value finish_func;
   Value finish_data;
   Value result;
   Value error;
   ComputeHeapRunFunc run_func;
   void *run_data;
   Heap *parent_heap;
   int from, to;
   int core_id;
   struct ComputeHeap *active_next;
   struct ComputeHeap *inactive_next;
   struct ComputeHeap *finished_next;
} ComputeHeap;

typedef struct {
   volatile int refcnt;
   int num_cores;
   int num_heaps;
   int quit;
   ComputeHeap *heaps;
   ComputeHeap *active_heaps;
   ComputeHeap *inactive_heaps;
   ComputeHeap *finished_heaps;
   pthread_mutex_t mutex;
   pthread_cond_t *conds;
   pthread_cond_t cond;
   int parallel_mode;
   int from, to, core_id;
} ComputeTasks;

typedef struct {
   Heap *heap;
   Value map;
} ParentHeap;

typedef struct ScriptHandle ScriptHandle;

typedef struct {
   volatile int refcnt;
   pthread_mutex_t mutex;
   Heap *heap;
   HeapCreateData *hc;
   ScriptHandle *handles;
} ScriptHeap;

typedef struct {
   ScriptHeap *script_heap;
} AsyncHeap;

struct ScriptHandle {
   Heap *heap;
   Value value;
   ScriptHeap *script_heap;
   Value script_heap_val;
   ScriptHandle *prev;
   ScriptHandle *next;
};

#ifdef __wasm__
typedef struct {
   ContinuationFunc func;
   void *data;
} BarrierContinuation;
#endif

typedef struct {
   int refcnt;
   int max_waiting;
   pthread_mutex_t mutex;
#ifdef __wasm__
   BarrierContinuation *conts;
#else
   pthread_cond_t *conds;
#endif
   int num_waiting;
   uint32_t counter_value;
   Heap *first_heap;
   Value first_marker;
} Barrier;

#ifdef __wasm__
typedef struct ChannelSender {
   void *channel;
   Heap *heap;
   Value value;
   ContinuationFunc wake_func;
   ContinuationResultFunc cont_func;
   void *cont_data;
   wasm_timer_t cancel_timer;
   struct ChannelSender *next;
} ChannelSender;

typedef struct ChannelReceiver {
   void *channel;
   Heap *heap;
   ContinuationFunc wake_func;
   ContinuationResultFunc cont_func;
   void *cont_data;
   wasm_timer_t cancel_timer;
   Value timeout_value;
   struct ChannelReceiver *next;
} ChannelReceiver;
#endif

struct ChannelEntry;
struct ChannelSet;

typedef struct Channel {
   int refcnt;
   int weakcnt;
   int size;
   union {
      struct {
         Heap *queue_heap;
         Value queue;
      };
      struct {
         Heap *send_heap;
         Value send_msg;
         int send_error;
      };
   };
   struct ChannelEntry *notify_entries;
   pthread_mutex_t mutex;
   pthread_cond_t send_cond;
   pthread_cond_t send_cond2;
   pthread_cond_t receive_cond;
#ifdef __wasm__
   ChannelSender *wasm_senders;
   ChannelReceiver *wasm_receivers;
#endif
} Channel;

typedef struct ChannelEntry {
   struct ChannelSet *set;
   Channel *channel;
   Value channel_val;
   Value key;
   struct ChannelEntry *next;
   struct ChannelEntry *notify_next;
} ChannelEntry;

typedef struct ChannelSet {
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   ChannelEntry **entries;
   int entries_cnt, entries_cap;
   ChannelEntry **notify_list;
   int notify_cnt, notify_cap;
#ifdef __wasm__
   void *cont_data;
#endif
} ChannelSet;

typedef struct {
   void (*integrate_func)(Heap *, void (*)(void *), void *);
   void (*process_func)(Heap *);
   int active;
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   pthread_mutex_t *wait_mutex;
   pthread_cond_t *wait_cond;
   int has_events;
   int sending_signal;
} AsyncIntegration;

#define NUM_HANDLE_TYPES 7
#define HANDLE_TYPE_TASK        (handles_offset+0)
#define HANDLE_TYPE_HEAP        (handles_offset+1)
#define HANDLE_TYPE_ASYNC_HEAP  (handles_offset+2)
#define HANDLE_TYPE_HANDLE      (handles_offset+3)
#define HANDLE_TYPE_BARRIER     (handles_offset+4)
#define HANDLE_TYPE_CHANNEL     (handles_offset+5)
#define HANDLE_TYPE_CHANNEL_SET (handles_offset+6)

static volatile int handles_offset = 0;
static volatile int heap_create_data_key;
static volatile int cur_task_key;
static volatile int compute_tasks_key;
static volatile int is_queue_heap_key;
static volatile int parent_heap_key;
static volatile int async_integration_key;

#define GET_PTR(ptr) (void *)((intptr_t)(ptr) & ~3)
#define GET_FLAGS(ptr) ((intptr_t)(ptr) & 3)
#define WITH_FLAGS(ptr, flags) (void *)((intptr_t)(ptr) | ((flags) & 3))

static volatile pthread_mutex_t *global_mutex;
static volatile int atomic_initialized = 0;
static pthread_mutex_t atomic_mutex[16];
static Heap *global_heap;
static Value global_hash;
#ifdef _WIN32
#define RECURSIVE_MUTEX_ATTR NULL
#else
static int recursive_mutex_attr_initialized = 0;
static pthread_mutexattr_t recursive_mutex_attr;
#define RECURSIVE_MUTEX_ATTR &recursive_mutex_attr
#endif


#ifdef _WIN32
static int pthread_mutex_init(pthread_mutex_t *mutex, void *attr)
{
   InitializeCriticalSection(mutex);
   return 0;
}

static int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
   DeleteCriticalSection(mutex);
   return 0;
}

static int pthread_mutex_lock(pthread_mutex_t *mutex)
{
   EnterCriticalSection(mutex);
   return 0;
}

static int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
   LeaveCriticalSection(mutex);
   return 0;
}

static int pthread_cond_init(pthread_cond_t *cond, void *attr)
{
   *cond = CreateEvent(NULL, FALSE, FALSE, NULL);
   if (!(*cond)) {
      return -1;
   }
   return 0;
}

static int pthread_cond_destroy(pthread_cond_t *cond)
{
   CloseHandle(*cond);
   *cond = NULL;
   return 0;
}

static int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
   LeaveCriticalSection(mutex);
   WaitForSingleObject(*cond, INFINITE);
   EnterCriticalSection(mutex);
   return 0;
}

static int pthread_cond_timedwait_relative(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t timeout)
{
   int ret = 0;
   LeaveCriticalSection(mutex);
   if (WaitForSingleObject(*cond, timeout/1000000) == WAIT_TIMEOUT) {
      ret = ETIMEDOUT;
   }
   EnterCriticalSection(mutex);
   return ret;
}

static int pthread_cond_signal(pthread_cond_t *cond)
{
   SetEvent(*cond);
   return 0;
}

#else

static int pthread_cond_timedwait_relative(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t timeout)
{
   struct timespec ts;
#if defined(__APPLE__)
   struct timeval tv;
   gettimeofday(&tv, NULL);
   ts.tv_sec = tv.tv_sec;
   ts.tv_nsec = tv.tv_usec * 1000;
#else
   clock_gettime(CLOCK_REALTIME, &ts);
#endif
   ts.tv_nsec += timeout % 1000000000;
   ts.tv_sec += ts.tv_nsec / 1000000000 + timeout / 1000000000;
   ts.tv_nsec %= 1000000000;
   return pthread_cond_timedwait(cond, mutex, &ts);
}
#endif


static uint64_t get_time()
{
#if defined(_WIN32)
   uint64_t freq, cnt;
   QueryPerformanceFrequency((LARGE_INTEGER *)&freq);
   QueryPerformanceCounter((LARGE_INTEGER *)&cnt);
   return cnt * 1000 / freq;
#elif defined(__linux__) || defined(__wasm__)
   struct timespec ts;
   
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
      ts.tv_sec = 0;
      ts.tv_nsec = 0;
   }

   return ts.tv_sec * 1000LL + (ts.tv_nsec + 500000) / 1000000;
#else
   struct timeval tv;

   if (gettimeofday(&tv, NULL) != 0) {
      tv.tv_sec = 0;
      tv.tv_usec = 0;
   }

   return tv.tv_sec * 1000LL + (tv.tv_usec + 500) / 1000;
#endif
}


static uint32_t rehash(uint32_t a)
{
   a = (a+0x7ed55d16) + (a<<12);
   a = (a^0xc761c23c) ^ (a>>19);
   a = (a+0x165667b1) + (a<<5);
   a = (a+0xd3a2646c) ^ (a<<9);
   a = (a+0xfd7046c5) + (a<<3);
   a = (a^0xb55a4f09) ^ (a>>16);
   return a;
}


static uint32_t hash_ptr(void *ptr)
{
   uint64_t value;
   
   if (sizeof(void *) == 8) {
      value = (uint64_t)(uintptr_t)ptr;
      return rehash((uint32_t)(value ^ (value >> 32)));
   }
   else {
      return rehash((uint32_t)(uintptr_t)ptr);
   }
}


static void *task_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   Task *task = p1;
   char buf[32];

   switch (op) {
      case HANDLE_OP_FREE:
         if (__sync_sub_and_fetch(&task->refcnt, 1) == 0) {
            if (task->comm_heap) {
               fixscript_free_heap(task->comm_heap);
            }
            free(task->fname);
            free(task->func_name);
            pthread_mutex_destroy(&task->mutex);
            pthread_cond_destroy(&task->cond);
            free(task);
         }
         break;

      case HANDLE_OP_COPY:
         (void)__sync_add_and_fetch(&task->refcnt, 1);
         return task;

      case HANDLE_OP_COMPARE:
         return (void *)(intptr_t)(p1 == p2);

      case HANDLE_OP_HASH:
         return p1;
         
      case HANDLE_OP_TO_STRING:
         snprintf(buf, sizeof(buf), "task(%p)", p1);
         return strdup(buf);
   }

   return NULL;
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   Value func_val;
   int num_params;
   Value *values;
   Task *task;
} ThreadData;

static void thread_finish(Heap *heap, Value result, Value error, void *data)
{
   ThreadData *td = data;
   Task *task = td->task;

   if (error.value) {
      fixscript_dump_value(heap, error, 1);
   }

   fixscript_set_heap_data(heap, cur_task_key, NULL, NULL);

   free(td->values);
   fixscript_unref(task->comm_heap, task->task_val);
   fixscript_collect_heap(task->comm_heap);
   task_handle_func(NULL, HANDLE_OP_FREE, task, NULL);
   fixscript_free_heap(heap);
   free(td);
}

static void thread_run(void *data)
{
   ThreadData *td = data;

   fixscript_call_async(td->heap, td->func_val, td->num_params, td->values, thread_finish, td);
}
#endif


#if defined(_WIN32)
static DWORD WINAPI thread_main(void *data)
#else
static void *thread_main(void *data)
#endif
{
   Task *task = data;
   Heap *heap;
   Script *script;
   Value params, *values = NULL, func_val, error;
   int err, num_params;
   char buf[128];
#ifdef __wasm__
   ThreadData *td;
#endif

   heap = task->hc.create_func(task->hc.create_data);
   if (!heap) {
      goto error;
   }

#ifdef __wasm__
   wasm_auto_suspend_heap(heap);
#endif

   script = task->hc.load_func(heap, task->fname, &error, task->hc.load_data);
   if (!script) {
      fprintf(stderr, "%s\n", fixscript_get_compiler_error(heap, error));
      goto error;
   }

   err = fixscript_clone_between(heap, task->comm_heap, task->start_params, &params, task->load_scripts? task->hc.load_func : fixscript_resolve_existing, task->hc.load_data, &error);
   if (err) {
      if (!error.value) {
         fixscript_error(heap, &error, err);
      }
      fixscript_dump_value(heap, error, 1);
      goto error;
   }

   fixscript_unref(task->comm_heap, task->start_params);

   err = fixscript_get_array_length(heap, params, &num_params);
   if (err) {
      fixscript_error(heap, &error, err);
      fixscript_dump_value(heap, error, 1);
      goto error;
   }

   values = malloc(num_params * sizeof(Value));
   if (!values) goto error;

   err = fixscript_get_array_range(heap, params, 0, num_params, values);
   if (err) {
      fixscript_error(heap, &error, err);
      fixscript_dump_value(heap, error, 1);
      goto error;
   }

   func_val = fixscript_get_function(heap, script, task->func_name);
   if (!func_val.value) {
      snprintf(buf, sizeof(buf), "can't find %s in %s", task->func_name, task->fname);
      fixscript_dump_value(heap, fixscript_create_error_string(heap, buf), 1);
      goto error;
   }

   err = fixscript_set_heap_data(heap, cur_task_key, task, NULL);
   if (err) {
      fixscript_dump_value(heap, fixscript_create_error_string(heap, "can't set current task"), 1);
      goto error;
   }

#ifdef __wasm__
   td = malloc(sizeof(ThreadData));
   td->heap = heap;
   td->func_val = func_val;
   td->num_params = num_params;
   td->values = values;
   td->task = task;
   wasm_sleep(0, thread_run, td);
   return NULL;
#else
   fixscript_call_args(heap, func_val, num_params, &error, values);
   if (error.value) {
      fixscript_dump_value(heap, error, 1);
   }

   fixscript_set_heap_data(heap, cur_task_key, NULL, NULL);
#endif

error:
   free(values);
   fixscript_unref(task->comm_heap, task->task_val);
   fixscript_collect_heap(task->comm_heap);
   task_handle_func(NULL, HANDLE_OP_FREE, task, NULL);
   if (heap) {
      fixscript_free_heap(heap);
   }
#if defined(_WIN32)
   return 0;
#else
   return NULL;
#endif
}


static Value task_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   char *fname = NULL, *func_name = NULL;
   Task *task = NULL;
   Value params_val, task_val, retval = fixscript_int(0);
   int len;
#if defined(_WIN32)
   HANDLE thread;
#else
   pthread_t thread;
#endif
   int err;

   if (num_params == 2) {
      err = fixscript_get_function_name(heap, params[0], &fname, &func_name, NULL);
      if (!err) {
         len = strlen(fname);
         if (len > 4 && strcmp(fname+(len-4), ".fix") == 0) {
            fname[len-4] = 0;
         }
      }
      params_val = params[1];
   }
   else {
      err = fixscript_get_string(heap, params[0], 0, -1, &fname, NULL);
      if (!err) {
         err = fixscript_get_string(heap, params[1], 0, -1, &func_name, NULL);
      }
      params_val = params[2];
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   task = calloc(1, sizeof(Task));
   if (!task) goto error;

   if (pthread_mutex_init(&task->mutex, RECURSIVE_MUTEX_ATTR) != 0) {
      free(task);
      task = NULL;
      goto error;
   }

   if (pthread_cond_init(&task->cond, NULL) != 0) {
      pthread_mutex_destroy(&task->mutex);
      free(task);
      task = NULL;
      goto error;
   }

   task->refcnt = 1;
   task->hc = *((HeapCreateData *)data);
   task->load_scripts = (num_params == 4 && params[3].value);
   task->comm_heap = fixscript_create_heap();
   if (!task->comm_heap) goto error;

   task->comm_arr = fixscript_create_array(task->comm_heap, 0);
   task->reply_arr = fixscript_create_array(task->comm_heap, 0);
   if (!task->comm_arr.value || !task->reply_arr.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }
   fixscript_ref(task->comm_heap, task->comm_arr);
   fixscript_ref(task->comm_heap, task->reply_arr);
   task->max_messages = 100;

   err = fixscript_clone_between(task->comm_heap, heap, params_val, &task->start_params, NULL, NULL, NULL);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }
   fixscript_ref(task->comm_heap, task->start_params);

   task->refcnt += 2;
   task_val = fixscript_create_value_handle(heap, HANDLE_TYPE_TASK, task, task_handle_func);
   task->task_val = fixscript_create_value_handle(task->comm_heap, HANDLE_TYPE_TASK, task, task_handle_func);
   if (!task_val.value || !task->task_val.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }
   fixscript_ref(task->comm_heap, task->task_val);

   task->fname = fname;
   task->func_name = func_name;

#if defined(_WIN32)
   thread = CreateThread(NULL, 0, thread_main, task, 0, NULL);
   if (!thread)
#else
   if (pthread_create(&thread, NULL, thread_main, task) != 0)
#endif
   {
      task->fname = NULL;
      task->func_name = NULL;
      *error = fixscript_create_error_string(heap, "can't create thread");
      goto error;
   }
#if defined(_WIN32)
   CloseHandle(thread);
#else
   pthread_detach(thread);
#endif
   task = NULL;
   fname = NULL;
   func_name = NULL;

   retval = task_val;

error:
   free(fname);
   free(func_name);
   if (task) {
      task_handle_func(NULL, HANDLE_OP_FREE, task, NULL);
   }
   return retval;
}


static Value task_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Task *task;
   Value task_val;
   int err;

   task = fixscript_get_heap_data(heap, cur_task_key);
   if (!task) {
      *error = fixscript_create_error_string(heap, "not in task thread");
      return fixscript_int(0);
   }

   if (pthread_mutex_lock(&task->mutex) != 0) {
      *error = fixscript_create_error_string(heap, "can't lock mutex");
      return fixscript_int(0);
   }

   err = fixscript_clone_between(heap, task->comm_heap, task->task_val, &task_val, NULL, NULL, NULL);

   pthread_mutex_unlock(&task->mutex);

   if (err) {
      return fixscript_error(heap, error, err);
   }
   return task_val;
}


#ifdef __wasm__
static void task_send_cont2(void *data)
{
   TaskSender *task_sender = data;
   Heap *heap = task_sender->heap;
   ContinuationResultFunc cont_func = task_sender->cont_func;
   void *cont_data = task_sender->cont_data;

   free(task_sender);
   cont_func(heap, fixscript_int(0), fixscript_int(0), cont_data);
}

static void task_send_cont(void *data)
{
   TaskSender *task_sender = data;
   Heap *heap = task_sender->heap;
   Value arr = task_sender->arr;
   Value msg = task_sender->msg;
   Task *task = task_sender->task;
   ContinuationResultFunc cont_func;
   void *cont_data;
   Value error;
   int err, len;
   TaskReceiver *r, **prev;
   
   err = fixscript_get_array_length(task->comm_heap, arr, &len);
   if (!err && len >= task->max_messages) {
      err = FIXSCRIPT_ERR_INVALID_ACCESS; // shouldn't happen
   }
   if (!err) {
      err = fixscript_clone_between(task->comm_heap, heap, msg, &msg, NULL, NULL, NULL);
   }
   if (!err) {
      err = fixscript_append_array_elem(task->comm_heap, arr, msg);
   }
   if (err) {
      cont_func = task_sender->cont_func;
      cont_data = task_sender->cont_data;
      free(task_sender);
      fixscript_error(heap, &error, err);
      cont_func(heap, fixscript_int(0), error, cont_data);
      return;
   }

   if (task->wasm_receivers) {
      wasm_sleep(0, task_send_cont2, task_sender);

      prev = &task->wasm_receivers;
      for (r = task->wasm_receivers; r; r = r->next) {
         if (!r->next) {
            *prev = NULL;
            r->wake_func(r);
            return;
         }
         prev = &r->next;
      }
   }
   else {
      cont_func = task_sender->cont_func;
      cont_data = task_sender->cont_data;
      free(task_sender);
      cont_func(heap, fixscript_int(0), fixscript_int(0), cont_data);
   }
}
#endif


static Value task_send(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Task *task;
   int in_task;
   Value arr, msg;
   int err, len;
#ifdef __wasm__
   TaskSender *task_sender;
   TaskReceiver *r, **prev;
   ContinuationFunc resume_func;
   void *resume_data;
#endif

   if (num_params == 1) {
      in_task = 1;
      task = fixscript_get_heap_data(heap, cur_task_key);
      msg = params[0];
   }
   else {
      in_task = 0;
      task = fixscript_get_handle(heap, params[0], HANDLE_TYPE_TASK, NULL);
      msg = params[1];
   }

   if (!task) {
      if (in_task) {
         *error = fixscript_create_error_string(heap, "not in task thread");
      }
      else {
         *error = fixscript_create_error_string(heap, "invalid task");
      }
      return fixscript_int(0);
   }

   if (pthread_mutex_lock(&task->mutex) != 0) {
      *error = fixscript_create_error_string(heap, "can't lock mutex");
      return fixscript_int(0);
   }

   if (in_task) {
      arr = task->reply_arr;
   }
   else {
      arr = task->comm_arr;
   }

   for (;;) {
      err = fixscript_get_array_length(task->comm_heap, arr, &len);
      if (err) break;
      if (len < task->max_messages) break;
      pthread_cond_wait(&task->cond, &task->mutex);
      #ifdef __wasm__
         task_sender = malloc(sizeof(TaskSender));
         if (!task_sender) {
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         task_sender->task = task;
         task_sender->heap = heap;
         task_sender->arr = arr;
         task_sender->msg = msg;
         task_sender->wake_func = task_send_cont;
         fixscript_suspend(heap, &task_sender->cont_func, &task_sender->cont_data);
         task_sender->next = task->wasm_senders;
         task->wasm_senders = task_sender;
         return fixscript_int(0);
      #endif
   }

   if (!err) {
      err = fixscript_clone_between(task->comm_heap, heap, msg, &msg, NULL, NULL, NULL);
   }
   if (!err) {
      err = fixscript_append_array_elem(task->comm_heap, arr, msg);
   }
   if (err) {
      pthread_mutex_unlock(&task->mutex);
      return fixscript_error(heap, error, err);
   }

   pthread_cond_signal(&task->cond);
   pthread_mutex_unlock(&task->mutex);

   #ifdef __wasm__
      if (task->wasm_receivers) {
         prev = &task->wasm_receivers;
         for (r = task->wasm_receivers; r; r = r->next) {
            if (!r->next) {
               fixscript_suspend_void(heap, &resume_func, &resume_data);
               wasm_sleep(0, resume_func, resume_data);
               *prev = NULL;
               wasm_sleep(0, r->wake_func, r);
               return fixscript_int(0);
            }
            prev = &r->next;
         }
      }
   #endif
   return fixscript_int(0);
}


#ifdef __wasm__
static void task_receive_cont(void *data)
{
   TaskReceiver *task_receiver = data;
   Task *task = task_receiver->task;
   Heap *heap = task_receiver->heap;
   Value arr = task_receiver->arr;
   ContinuationResultFunc cont_func;
   void *cont_data;
   Value error = fixscript_int(0), msg;
   int len, err;

   if (task_receiver->cancel_timer != WASM_TIMER_NULL) {
      wasm_timer_stop(task_receiver->cancel_timer);
   }

   err = fixscript_get_array_length(task->comm_heap, arr, &len);
   if (!err && len == 0) {
      err = FIXSCRIPT_ERR_INVALID_ACCESS; // shouldn't happen
   }
   if (err) {
      cont_func = task_receiver->cont_func;
      cont_data = task_receiver->cont_data;
      free(task_receiver);
      fixscript_error(heap, &error, err);
      cont_func(heap, fixscript_int(0), error, cont_data);
      return;
   }

   err = fixscript_get_array_elem(task->comm_heap, arr, 0, &msg);
   if (!err) {
      err = fixscript_copy_array(task->comm_heap, arr, 0, arr, 1, len-1);
   }
   if (!err) {
      err = fixscript_set_array_length(task->comm_heap, arr, len-1);
   }
   if (!err) {
      err = fixscript_clone_between(heap, task->comm_heap, msg, &msg, task->load_scripts? task->hc.load_func : fixscript_resolve_existing, task->hc.load_data, &error);
   }

   fixscript_collect_heap(task->comm_heap);

   cont_func = task_receiver->cont_func;
   cont_data = task_receiver->cont_data;
   free(task_receiver);

   if (err) {
      if (!error.value) {
         fixscript_error(heap, &error, err);
      }
      cont_func(heap, fixscript_int(0), error, cont_data);
      return;
   }

   cont_func(heap, msg, fixscript_int(0), cont_data);
}

static void task_receive_cancel(void *data)
{
   TaskReceiver *task_receiver = data;
   Task *task = task_receiver->task;
   Heap *heap = task_receiver->heap;
   TaskReceiver *r, **prev;
   ContinuationResultFunc cont_func;
   void *cont_data;

   prev = &task->wasm_receivers;
   for (r = task->wasm_receivers; r; r = r->next) {
      if (r == task_receiver) {
         *prev = r->next;
         break;
      }
      prev = &r->next;
   }

   cont_func = task_receiver->cont_func;
   cont_data = task_receiver->cont_data;
   free(task_receiver);
   cont_func(heap, fixscript_int(0), fixscript_int(0), cont_data);
}
#endif


static Value task_receive(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int wait = (data != NULL);
   int in_task;
   Task *task;
   Value arr, msg;
   int timeout;
   int err, len;
#ifdef __wasm__
   TaskReceiver *task_receiver;
   TaskSender *s, **prev;
#else
   uint64_t wait_until = 0;
#endif

   if (wait) {
      if (num_params == 1) {
         in_task = 1;
         task = fixscript_get_heap_data(heap, cur_task_key);
         timeout = params[0].value;
      }
      else {
         in_task = 0;
         task = fixscript_get_handle(heap, params[0], HANDLE_TYPE_TASK, NULL);
         timeout = params[1].value;
      }
   }
   else {
      if (num_params == 0) {
         in_task = 1;
         task = fixscript_get_heap_data(heap, cur_task_key);
      }
      else {
         in_task = 0;
         task = fixscript_get_handle(heap, params[0], HANDLE_TYPE_TASK, NULL);
      }
      timeout = -1;
   }

   if (!task) {
      if (in_task) {
         *error = fixscript_create_error_string(heap, "not in task thread");
      }
      else {
         *error = fixscript_create_error_string(heap, "invalid task");
      }
      return fixscript_int(0);
   }

   if (pthread_mutex_lock(&task->mutex) != 0) {
      *error = fixscript_create_error_string(heap, "can't lock mutex");
      return fixscript_int(0);
   }

   if (in_task) {
      arr = task->comm_arr;
   }
   else {
      arr = task->reply_arr;
   }

   #ifndef __wasm__
      if (timeout > 0) {
         wait_until = get_time() + timeout;
      }
   #endif

   for (;;) {
      err = fixscript_get_array_length(task->comm_heap, arr, &len);
      if (err) {
         pthread_mutex_unlock(&task->mutex);
         return fixscript_error(heap, error, err);
      }
      if (len > 0) break;

      #ifdef __wasm__
         if (timeout == 0) {
            return fixscript_int(0);
         }

         task_receiver = malloc(sizeof(TaskReceiver));
         if (!task_receiver) {
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         task_receiver->task = task;
         task_receiver->heap = heap;
         task_receiver->arr = arr;
         task_receiver->wake_func = task_receive_cont;
         fixscript_suspend(heap, &task_receiver->cont_func, &task_receiver->cont_data);
         task_receiver->cancel_timer = WASM_TIMER_NULL;
         task_receiver->next = task->wasm_receivers;
         task->wasm_receivers = task_receiver;

         if (task->wasm_senders) {
            prev = &task->wasm_senders;
            for (s = task->wasm_senders; s; s = s->next) {
               if (!s->next) {
                  *prev = NULL;
                  wasm_sleep(0, s->wake_func, s);
                  return fixscript_int(0);
               }
               prev = &s->next;
            }
         }

         if (timeout > 0) {
            task_receiver->cancel_timer = wasm_sleep(timeout, task_receive_cancel, task_receiver);
         }
         return fixscript_int(0);
      #else
         if (timeout < 0) {
            pthread_cond_wait(&task->cond, &task->mutex);
         }
         else {
            if (timeout > 0) {
               timeout = wait_until - get_time();
            }
            if (timeout <= 0 || pthread_cond_timedwait_relative(&task->cond, &task->mutex, timeout*1000000LL) == ETIMEDOUT) {
               pthread_mutex_unlock(&task->mutex);
               return fixscript_int(0);
            }
         }
      #endif
   }

   err = fixscript_get_array_elem(task->comm_heap, arr, 0, &msg);
   if (!err) {
      err = fixscript_copy_array(task->comm_heap, arr, 0, arr, 1, len-1);
   }
   if (!err) {
      err = fixscript_set_array_length(task->comm_heap, arr, len-1);
   }
   if (!err) {
      err = fixscript_clone_between(heap, task->comm_heap, msg, &msg, task->load_scripts? task->hc.load_func : fixscript_resolve_existing, task->hc.load_data, error);
   }

   fixscript_collect_heap(task->comm_heap);

   pthread_cond_signal(&task->cond);
   pthread_mutex_unlock(&task->mutex);

   if (err) {
      if (!error->value) {
         fixscript_error(heap, error, err);
      }
      return fixscript_int(0);
   }
   return msg;
}


static Value sleep_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#if defined(_WIN32)
   Sleep(params[0].value);
#elif defined(__wasm__)
   ContinuationFunc cont_func;
   void *cont_data;

   fixscript_suspend_void(heap, &cont_func, &cont_data);
   wasm_sleep(params[0].value, cont_func, cont_data);
#else
   usleep(params[0].value * 1000);
#endif
   return fixscript_int(0);
}


#ifndef __wasm__
static void unref_compute_tasks(ComputeTasks *tasks)
{
   int i;

   if (__sync_sub_and_fetch(&tasks->refcnt, 1) == 0) {
      for (i=0; i<tasks->num_heaps; i++) {
         fixscript_free_heap(tasks->heaps[i].heap);
      }
      for (i=0; i<tasks->num_cores; i++) {
         pthread_cond_destroy(&tasks->conds[i]);
      }
      free(tasks->conds);
      pthread_mutex_destroy(&tasks->mutex);
      pthread_cond_destroy(&tasks->cond);
      free(tasks->heaps);
      free(tasks);
   }
}
#endif


#ifndef __wasm__
typedef struct {
   ComputeTasks *tasks;
   int id;
} ComputeThreadData;

#if defined(_WIN32)
static DWORD WINAPI compute_thread_main(void *data)
#else
static void *compute_thread_main(void *data)
#endif
{
   ComputeThreadData *ctd = data;
   ComputeTasks *tasks;
   ComputeHeap *heap;
   ParentHeap parent_heap;
   pthread_cond_t *cond;
   int id, err;

   tasks = ctd->tasks;
   id = ctd->id;
   cond = &tasks->conds[id];
   free(ctd);

   pthread_mutex_lock(&tasks->mutex);
   for (;;) {
      while (!tasks->quit && !tasks->active_heaps) {
         pthread_cond_wait(cond, &tasks->mutex);
      }
      if (tasks->quit) {
         break;
      }
      heap = tasks->active_heaps;
      tasks->active_heaps = heap->active_next;
      heap->active_next = NULL;
      pthread_mutex_unlock(&tasks->mutex);

      if (heap->run_func) {
         heap->run_func(heap->heap, heap->core_id, heap->run_data);
      }
      else {
         if (heap->parent_heap) {
            parent_heap.heap = heap->parent_heap;
            parent_heap.map = fixscript_int(0);

            err = fixscript_set_heap_data(heap->heap, parent_heap_key, &parent_heap, NULL);
            if (err) {
               heap->result = fixscript_error(heap->heap, &heap->error, err);
            }
            else {
               heap->result = fixscript_call(heap->heap, heap->process_func, 4, &heap->error, heap->process_data, fixscript_int(heap->from), fixscript_int(heap->to), fixscript_int(heap->core_id));
               fixscript_unref(heap->heap, parent_heap.map);
               fixscript_set_heap_data(heap->heap, parent_heap_key, NULL, NULL);
            }
         }
         else {
            heap->result = fixscript_call(heap->heap, heap->process_func, 1, &heap->error, heap->process_data);
         }
         fixscript_unref(heap->heap, heap->process_data);
         fixscript_ref(heap->heap, heap->result);
         fixscript_ref(heap->heap, heap->error);
      }
      fixscript_collect_heap(heap->heap);

      pthread_mutex_lock(&tasks->mutex);
      if (heap->run_func) {
         heap->inactive_next = tasks->inactive_heaps;
         tasks->inactive_heaps = heap;
         heap->run_func = NULL;
      }
      else {
         heap->finished_next = tasks->finished_heaps;
         tasks->finished_heaps = heap;
         heap->parent_heap = NULL;
      }
      pthread_cond_signal(&tasks->cond);
   }
   pthread_mutex_unlock(&tasks->mutex);

   unref_compute_tasks(tasks);

#if defined(_WIN32)
   return 0;
#else
   return NULL;
#endif
}
#endif


#ifndef __wasm__
static int get_number_of_cores()
{
#if defined(_WIN32)
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   return si.dwNumberOfProcessors;
#else
   return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}
#endif


#ifndef __wasm__
static void free_compute_tasks(void *data)
{
   ComputeTasks *tasks = data;
   int i;

   pthread_mutex_lock(&tasks->mutex);
   tasks->quit = 1;
   tasks->active_heaps = NULL;
   for (i=0; i<tasks->num_cores; i++) {
      pthread_cond_signal(&tasks->conds[i]);
   }
   pthread_mutex_unlock(&tasks->mutex);

   unref_compute_tasks(tasks);
}
#endif


#ifndef __wasm__
static ComputeTasks *get_compute_tasks(Heap *heap, HeapCreateData *hc)
{
   ComputeTasks *tasks;
   int i, err;
   ComputeThreadData *ctd;
#if defined(_WIN32)
   HANDLE thread;
#else
   pthread_t thread;
#endif

   tasks = fixscript_get_heap_data(heap, compute_tasks_key);
   if (tasks) {
      return tasks;
   }

   if (!hc) {
      return NULL;
   }

   tasks = calloc(1, sizeof(ComputeTasks));
   if (!tasks) {
      return NULL;
   }

   tasks->refcnt = 1;
   tasks->num_cores = get_number_of_cores();
   if (tasks->num_cores < 1) tasks->num_cores = 1;
   tasks->num_heaps = tasks->num_cores > 1? tasks->num_cores+1 : 1;

   tasks->heaps = calloc(tasks->num_heaps, sizeof(ComputeHeap));
   if (!tasks->heaps) {
      free(tasks);
      return NULL;
   }

   if (pthread_mutex_init(&tasks->mutex, RECURSIVE_MUTEX_ATTR) != 0) {
      free(tasks->heaps);
      free(tasks);
      return NULL;
   }
   
   if (pthread_cond_init(&tasks->cond, NULL) != 0) {
      pthread_mutex_destroy(&tasks->mutex);
      free(tasks->heaps);
      free(tasks);
      return NULL;
   }

   tasks->conds = calloc(tasks->num_cores, sizeof(pthread_cond_t));
   if (!tasks->conds) {
      pthread_mutex_destroy(&tasks->mutex);
      pthread_cond_destroy(&tasks->cond);
      free(tasks->heaps);
      free(tasks);
      return NULL;
   }

   for (i=0; i<tasks->num_cores; i++) {
      if (pthread_cond_init(&tasks->conds[i], NULL) != 0) {
         while (--i >= 0) {
            pthread_cond_destroy(&tasks->conds[i]);
         }
         pthread_mutex_destroy(&tasks->mutex);
         pthread_cond_destroy(&tasks->cond);
         free(tasks->heaps);
         free(tasks->conds);
         free(tasks);
         return NULL;
      }
   }

   for (i=0; i<tasks->num_heaps; i++) {
      tasks->heaps[i].heap = hc->create_func(hc->create_data);
      if (!tasks->heaps[i].heap) {
         while (--i >= 0) {
            fixscript_free_heap(tasks->heaps[i].heap);
         }
         for (i=0; i<tasks->num_cores; i++) {
            pthread_cond_destroy(&tasks->conds[i]);
         }
         pthread_mutex_destroy(&tasks->mutex);
         pthread_cond_destroy(&tasks->cond);
         free(tasks->heaps);
         free(tasks->conds);
         free(tasks);
         return NULL;
      }
   }
   
   for (i=0; i<tasks->num_cores; i++) {
      ctd = calloc(1, sizeof(ComputeThreadData));
      if (!ctd) {
         free_compute_tasks(tasks);
         return NULL;
      }
      
      ctd->tasks = tasks;
      ctd->id = i;
      (void)__sync_add_and_fetch(&tasks->refcnt, 1);

      #if defined(_WIN32)
      thread = CreateThread(NULL, 0, compute_thread_main, ctd, 0, NULL);
      if (!thread)
      #else
      if (pthread_create(&thread, NULL, compute_thread_main, ctd) != 0)
      #endif
      {
         (void)__sync_sub_and_fetch(&tasks->refcnt, 1);
         free(ctd);
         free_compute_tasks(tasks);
         return NULL;
      }
      #if defined(_WIN32)
      CloseHandle(thread);
      #else
      pthread_detach(thread);
      #endif
   }

   for (i=tasks->num_heaps-1; i>=0; i--) {
      tasks->heaps[i].inactive_next = tasks->inactive_heaps;
      tasks->inactive_heaps = &tasks->heaps[i];
   }

   err = fixscript_set_heap_data(heap, compute_tasks_key, tasks, free_compute_tasks);
   if (err) {
      return NULL;
   }
   return tasks;
}
#endif


#ifndef __wasm__
static void finish_tasks(Heap *heap, Value *error, ComputeTasks *tasks)
{
   ComputeHeap *cheap;
   Value result;
   int err;

   for (;;) {
      cheap = tasks->finished_heaps;
      if (!cheap) break;

      tasks->finished_heaps = cheap->finished_next;
      cheap->finished_next = NULL;

      if (cheap->error.value) {
         err = fixscript_clone_between(heap, cheap->heap, cheap->error, error, fixscript_resolve_existing, NULL, NULL);
         if (err) {
            fixscript_error(heap, error, err);
         }
         else {
            *error = fixscript_create_error(heap, *error);
         }
         fixscript_unref(cheap->heap, cheap->result);
         fixscript_unref(cheap->heap, cheap->error);
         fixscript_collect_heap(cheap->heap);
         cheap->inactive_next = tasks->inactive_heaps;
         tasks->inactive_heaps = cheap;
         return;
      }

      err = fixscript_clone_between(heap, cheap->heap, cheap->result, &result, fixscript_resolve_existing, NULL, NULL);
      fixscript_unref(cheap->heap, cheap->result);
      fixscript_collect_heap(cheap->heap);
      if (err) {
         fixscript_error(heap, error, err);
         cheap->inactive_next = tasks->inactive_heaps;
         tasks->inactive_heaps = cheap;
         return;
      }

      if (cheap->finish_func.value) {
         pthread_mutex_unlock(&tasks->mutex);
         fixscript_call(heap, cheap->finish_func, 2, error, cheap->finish_data, result);
         fixscript_unref(heap, cheap->finish_data);
         pthread_mutex_lock(&tasks->mutex);
         cheap->inactive_next = tasks->inactive_heaps;
         tasks->inactive_heaps = cheap;
         if (error->value) {
            return;
         }
      }
      else {
         cheap->inactive_next = tasks->inactive_heaps;
         tasks->inactive_heaps = cheap;
      }
   }
}
#endif


#ifdef __wasm__
typedef struct {
   Value finish_func, finish_data;
   ContinuationResultFunc cont_func;
   void *cont_data;
} ComputeTaskRunCont;

static void compute_task_run_cont2(Heap *heap, Value result, Value error, void *data)
{
   ComputeTaskRunCont *cont = data;
   ContinuationResultFunc cont_func;
   void *cont_data;

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);
   cont_func(heap, fixscript_int(0), error, cont_data);
}

static void compute_task_run_cont(Heap *heap, Value result, Value error, void *data)
{
   ComputeTaskRunCont *cont = data;
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (!error.value && cont->finish_func.value) {
      fixscript_call_async(heap, cont->finish_func, 2, (Value[]) { cont->finish_data, result }, compute_task_run_cont2, cont);
      return;
   }

   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);
   cont_func(heap, fixscript_int(0), error, cont_data);
}
#endif


static Value compute_task_run(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   ComputeTaskRunCont *cont;

   cont = malloc(sizeof(ComputeTaskRunCont));
   if (num_params == 2) {
      cont->finish_func = fixscript_int(0);
      cont->finish_data = fixscript_int(0);
   }
   else {
      cont->finish_func = params[2];
      cont->finish_data = params[3];
   }

   fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
   fixscript_call_async(heap, params[0], 1, &params[1], compute_task_run_cont, cont);
   return fixscript_int(0);
#else
   HeapCreateData *hc = data;
   ComputeTasks *tasks;
   ComputeHeap *cheap;
   int i, err;

   tasks = get_compute_tasks(heap, hc);
   if (!tasks) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   if (!params[0].value) {
      *error = fixscript_create_error_string(heap, "must provide process function");
      return fixscript_int(0);
   }

   pthread_mutex_lock(&tasks->mutex);
   
   for (;;) {
      if (tasks->inactive_heaps) break;

      finish_tasks(heap, error, tasks);
      if (error->value) {
         pthread_mutex_unlock(&tasks->mutex);
         return fixscript_int(0);
      }
   
      if (tasks->inactive_heaps) break;

      pthread_cond_wait(&tasks->cond, &tasks->mutex);
   }

   cheap = tasks->inactive_heaps;
   tasks->inactive_heaps = cheap->inactive_next;

   pthread_mutex_unlock(&tasks->mutex);

   err = fixscript_clone_between(cheap->heap, heap, params[0], &cheap->process_func, hc->load_func, hc->load_data, error);
   if (!err) {
      err = fixscript_clone_between(cheap->heap, heap, params[1], &cheap->process_data, hc->load_func, hc->load_data, error);
   }
   if (err) {
      if (error->value) {
         if (fixscript_clone_between(heap, cheap->heap, *error, error, NULL, NULL, NULL) != FIXSCRIPT_SUCCESS) {
            *error = fixscript_int(0);
         }
      }
      if (error->value) {
         *error = fixscript_create_error(heap, *error);
      }
      else {
         fixscript_error(heap, error, err);
      }
      pthread_mutex_lock(&tasks->mutex);
      cheap->inactive_next = tasks->inactive_heaps;
      tasks->inactive_heaps = cheap;
      pthread_mutex_unlock(&tasks->mutex);
      return fixscript_int(0);
   }
   
   if (num_params > 2 && params[2].value) {
      cheap->finish_func = params[2];
      cheap->finish_data = params[3];
   }
   else {
      cheap->finish_func = fixscript_int(0);
      cheap->finish_data = fixscript_int(0);
   }

   fixscript_ref(cheap->heap, cheap->process_data);
   fixscript_ref(heap, cheap->finish_data);

   if (tasks->parallel_mode) {
      cheap->parent_heap = heap;
      cheap->from = tasks->from;
      cheap->to = tasks->to;
      cheap->core_id = tasks->core_id;
   }

   pthread_mutex_lock(&tasks->mutex);
   cheap->active_next = tasks->active_heaps;
   tasks->active_heaps = cheap;
   for (i=0; i<tasks->num_cores; i++) {
      pthread_cond_signal(&tasks->conds[i]);
   }
   pthread_mutex_unlock(&tasks->mutex);

   return fixscript_int(0);
#endif
}


static Value compute_task_check_finished(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   return fixscript_int(0);
#else
   ComputeTasks *tasks;

   tasks = get_compute_tasks(heap, NULL);
   if (!tasks) {
      return fixscript_int(0);
   }

   pthread_mutex_lock(&tasks->mutex);
   finish_tasks(heap, error, tasks);
   pthread_mutex_unlock(&tasks->mutex);
   return fixscript_int(0);
#endif
}


static Value compute_task_finish_all(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   return fixscript_int(0);
#else
   ComputeTasks *tasks;
   ComputeHeap *cheap;
   int num_inactive;

   tasks = get_compute_tasks(heap, NULL);
   if (!tasks) {
      return fixscript_int(0);
   }

   pthread_mutex_lock(&tasks->mutex);
   
   for (;;) {
      num_inactive = 0;
      cheap = tasks->inactive_heaps;
      while (cheap) {
         num_inactive++;
         cheap = cheap->inactive_next;
      }
      if (num_inactive == tasks->num_heaps) break;

      while (!tasks->finished_heaps) {
         pthread_cond_wait(&tasks->cond, &tasks->mutex);
      }

      while (tasks->finished_heaps) {
         finish_tasks(heap, error, tasks);
         if (error->value) {
            pthread_mutex_unlock(&tasks->mutex);
            return fixscript_int(0);
         }
      }
   }

   pthread_mutex_unlock(&tasks->mutex);
   return fixscript_int(0);
#endif
}


static Value compute_task_get_core_count(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int(fixtask_get_core_count(heap));
}


static Value compute_task_run_parallel(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   ContinuationResultFunc cont_func;
   void *cont_data;

   fixscript_suspend(heap, &cont_func, &cont_data);
   fixscript_call_async(heap, params[num_params-2], 4, (Value[]) { params[num_params-1], params[0], params[1], fixscript_int(0) }, cont_func, cont_data);
   return fixscript_int(0);
#else
   HeapCreateData *hc = data;
   ComputeTasks *tasks;
   Value params2[2], error2 = fixscript_int(0);
   int i, from, to, min_iters, num_cores, iters_per_core;

   tasks = get_compute_tasks(heap, hc);
   if (!tasks) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   from = params[0].value;
   to = params[1].value;
   if (num_params == 5) {
      min_iters = params[2].value;
      if (min_iters < 1) {
         min_iters = 1;
      }
   }
   else {
      min_iters = 1;
   }
   num_cores = tasks->num_cores;

   if (from >= to) {
      return fixscript_int(0);
   }

   if (((to - from) >> 1) < min_iters || num_cores == 1) {
      fixscript_call(heap, params[num_params-2], 4, error, params[num_params-1], fixscript_int(from), fixscript_int(to), fixscript_int(0));
      return fixscript_int(0);
   }

   compute_task_finish_all(heap, error, 0, NULL, NULL);
   if (error->value) {
      return fixscript_int(0);
   }

   params2[0] = params[num_params-2];
   params2[1] = params[num_params-1];
   tasks->parallel_mode = 1;

   if (to - from < min_iters * num_cores) {
      num_cores = (to - from) / min_iters;
      min_iters = (to - from + num_cores - 1) / num_cores;
   }
   iters_per_core = (to - from) / num_cores;
   if (iters_per_core < min_iters) {
      iters_per_core = min_iters;
   }

   for (i=0; i<num_cores; i++) {
      tasks->core_id = i;
      tasks->from = from + iters_per_core * i;
      tasks->to = tasks->from + iters_per_core;
      if (i == num_cores-1 && tasks->to < to) {
         tasks->to = to;
      }
      if (tasks->to > to) {
         tasks->to = to;
      }
      compute_task_run(heap, error, 2, params2, hc);
      if (error->value) {
         break;
      }
   }

   tasks->parallel_mode = 0;
   compute_task_finish_all(heap, &error2, 0, NULL, NULL);
   if (!error->value) {
      *error = error2;
   }
   return fixscript_int(0);
#endif
}


static int get_parent_ref(Heap *heap, Value *error, Heap **parent_heap_out, Value *value)
{
   ParentHeap *parent_heap;

   parent_heap = fixscript_get_heap_data(heap, parent_heap_key);
   if (parent_heap_out) {
      if (parent_heap) {
         *parent_heap_out = parent_heap->heap;
      }
      else {
         *parent_heap_out = heap;
      }
   }

   value->is_array = 1;

   if (fixscript_is_protected(parent_heap? parent_heap->heap : heap, *value)) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
      return 0;
   }
   return 1;
}


static Value parent_ref_length(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value value = params[0];
   int err, len;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   err = fixscript_get_array_length(parent_heap, value, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(len);
}


static Value parent_ref_array_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value value = params[0];
   int err;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   err = fixscript_get_array_elem(parent_heap, value, params[1].value, &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (fixscript_is_float(value)) {
      return value;
   }
   return fixscript_int(value.value);
}


static Value parent_ref_is_check(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int type = (int)(intptr_t)data;
   Heap *parent_heap;
   Value value = params[0];
   int ret = 0;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   switch (type) {
      case CHECK_ARRAY:   ret = fixscript_is_array(parent_heap, value); break;
      case CHECK_STRING:  ret = fixscript_is_string(parent_heap, value); break;
      case CHECK_HASH:    ret = fixscript_is_hash(parent_heap, value); break;
      case CHECK_SHARED:  ret = fixscript_is_shared_array(parent_heap, value); break;
      case CHECK_FUNCREF: ret = fixscript_is_func_ref(parent_heap, value); break;
      case CHECK_WEAKREF: ret = fixscript_is_weak_ref(parent_heap, value); break;
      case CHECK_HANDLE:  ret = fixscript_is_handle(parent_heap, value); break;
   }

   return fixscript_int(ret);
}


static Value parent_ref_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int cache = data != NULL;
   ParentHeap *parent_heap;
   Value value = params[0], value2;
   int err;

   parent_heap = fixscript_get_heap_data(heap, parent_heap_key);
   if (!parent_heap) {
      value.is_array = 1;
      if (fixscript_is_protected(heap, value)) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
      }
      if (!fixscript_is_array(heap, value) && !fixscript_is_hash(heap, value) && !fixscript_is_func_ref(heap, value) && !fixscript_is_handle(heap, value)) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
      }
      return value;
   }

   if (cache) {
      if (!parent_heap->map.value) {
         parent_heap->map = fixscript_create_hash(heap);
         if (!parent_heap->map.value) {
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         fixscript_ref(heap, parent_heap->map);
      }

      err = fixscript_get_hash_elem(heap, parent_heap->map, fixscript_int(value.value), &value2);
      if (err != FIXSCRIPT_ERR_KEY_NOT_FOUND) {
         if (err) {
            return fixscript_error(heap, error, err);
         }
         return value2;
      }
   }

   if (!get_parent_ref(heap, error, NULL, &value)) {
      return fixscript_int(0);
   }

   err = fixscript_clone_between(heap, parent_heap->heap, value, &value2, fixscript_resolve_existing, NULL, error);
   if (err) {
      if (!error->value) {
         fixscript_error(heap, error, err);
      }
      return fixscript_int(0);
   }

   if (cache) {
      err = fixscript_set_hash_elem(heap, parent_heap->map, fixscript_int(value.value), value2);
      if (err) {
         return fixscript_error(heap, error, err);
      }
   }

   return value2;
}


static Value parent_ref_get_shared_count(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value value = params[0];
   SharedArrayHandle *sah;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   sah = fixscript_get_shared_array_handle(parent_heap, value, -1, NULL);
   if (!sah) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_INVALID_ACCESS);
   }

   return fixscript_int(fixscript_get_shared_array_reference_count(sah));
}


static Value parent_ref_get_element_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value value = params[0];
   int err, elem_size;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   err = fixscript_get_array_element_size(parent_heap, value, &elem_size);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(elem_size);
}


static Value parent_ref_copy_to(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value src = params[0];
   Value dest = params[1];
   int dest_off = params[2].value;
   int src_off = params[3].value;
   int count = params[4].value;
   Value values[128];
   int err, i, cnt;

   if (!get_parent_ref(heap, error, &parent_heap, &src)) {
      return fixscript_int(0);
   }

   if (dest_off < 0 || src_off < 0 || count < 0) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
   }

   while (count > 0) {
      cnt = count;
      if (cnt > sizeof(values)/sizeof(Value)) {
         cnt = sizeof(values)/sizeof(Value);
      }

      err = fixscript_get_array_range(parent_heap, src, src_off, cnt, values);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      if (parent_heap != heap) {
         for (i=0; i<cnt; i++) {
            err = fixscript_clone_between(heap, parent_heap, values[i], &values[i], fixscript_resolve_existing, NULL, error);
            if (err) {
               if (!error->value) {
                  fixscript_error(heap, error, err);
               }
               return fixscript_int(0);
            }
         }
      }

      err = fixscript_set_array_range(heap, dest, dest_off, cnt, values);
      if (err) {
         return fixscript_error(heap, error, err);
      }

      src_off += cnt;
      dest_off += cnt;
      count -= cnt;
   }

   return fixscript_int(0);
}


static Value parent_ref_extract(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value value = params[0], new_arr, params2[5];
   int off = params[1].value;
   int count = params[2].value;
   int err;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   if (fixscript_is_string(parent_heap, value)) {
      new_arr = fixscript_create_string(heap, NULL, 0);
      if (new_arr.value) {
         err = fixscript_set_array_length(heap, new_arr, count);
         if (err) {
            return fixscript_error(heap, error, err);
         }
      }
   }
   else {
      new_arr = fixscript_create_array(heap, count);
   }
   if (!new_arr.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   params2[0] = params[0];
   params2[1] = new_arr;
   params2[2] = fixscript_int(0);
   params2[3] = fixscript_int(off);
   params2[4] = fixscript_int(count);
   parent_ref_copy_to(heap, error, 5, params2, NULL);

   return new_arr;
}


static Value parent_ref_weakref_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value value = params[0];
   int err;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   err = fixscript_get_weak_ref(parent_heap, value, &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(value.value);
}


static Value parent_ref_hash_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value value = params[0];
   int err;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   err = fixscript_get_hash_elem_between(parent_heap, value, heap, params[1], &value);
   if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
      return params[2];
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (fixscript_is_float(value)) {
      return value;
   }
   return fixscript_int(value.value);
}


static Value parent_ref_hash_contains(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value value = params[0];
   int err;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   err = fixscript_get_hash_elem_between(parent_heap, value, heap, params[1], &value);
   if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
      return fixscript_int(0);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(1);
}


static Value parent_ref_to_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Heap *parent_heap;
   Value value = params[0];
   char *s;
   int newlines = (num_params == 2? params[1].value : 0);
   int err, len;

   if (!get_parent_ref(heap, error, &parent_heap, &value)) {
      return fixscript_int(0);
   }

   err = fixscript_to_string(parent_heap, value, newlines, &s, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   value = fixscript_create_string(heap, s, len);
   free(s);
   if (!value.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return value;
}


static void unref_script_heap(ScriptHeap *script_heap)
{
   if (__sync_sub_and_fetch(&script_heap->refcnt, 1) == 0) {
      pthread_mutex_destroy(&script_heap->mutex);
      free(script_heap);
   }
}


static void *script_heap_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   ScriptHeap *script_heap = p1;
   ScriptHandle *h;
   char buf[64];

   switch (op) {
      case HANDLE_OP_FREE:
         pthread_mutex_lock(&script_heap->mutex);
         if (script_heap->heap) {
            fixscript_free_heap(script_heap->heap);
            script_heap->heap = NULL;
         }
         pthread_mutex_unlock(&script_heap->mutex);
         unref_script_heap(script_heap);
         break;
         
      case HANDLE_OP_TO_STRING:
         if (script_heap->heap) {
            #ifdef _WIN32
            snprintf(buf, sizeof(buf), "heap(%p,size=%I64d)", p1, fixscript_heap_size(script_heap->heap));
            #else
            snprintf(buf, sizeof(buf), "heap(%p,size=%lld)", p1, fixscript_heap_size(script_heap->heap));
            #endif
         }
         else {
            snprintf(buf, sizeof(buf), "heap(%p,<destroyed>)", p1);
         }
         return strdup(buf);

      case HANDLE_OP_MARK_REFS:
         for (h = script_heap->handles; h; h = h->next) {
            fixscript_mark_ref(heap, h->value);
         }
         break;
   }

   return NULL;
}


static void *async_heap_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   AsyncHeap *async_heap = p1, *copy;
   char buf[64];

   switch (op) {
      case HANDLE_OP_FREE:
         unref_script_heap(async_heap->script_heap);
         free(async_heap);
         break;
         
      case HANDLE_OP_COPY:
         copy = calloc(1, sizeof(AsyncHeap));
         if (!copy) return NULL;
         copy->script_heap = async_heap->script_heap;
         (void)__sync_add_and_fetch(&copy->script_heap->refcnt, 1);
         return copy;

      case HANDLE_OP_COMPARE:
         return (void *)(intptr_t)(async_heap->script_heap == ((AsyncHeap *)p2)->script_heap);

      case HANDLE_OP_HASH:
         return async_heap->script_heap;

      case HANDLE_OP_TO_STRING:
         snprintf(buf, sizeof(buf), "async_heap(%p)", async_heap->script_heap);
         return strdup(buf);
   }

   return NULL;
}


static void *script_handle_handle_func(Heap *script_heap, int op, void *p1, void *p2)
{
   ScriptHandle *script_handle = p1;
   Heap *heap = script_handle->heap;
   Value func, error, ret;
   char *s;
   int err;

   switch (op) {
      case HANDLE_OP_FREE:
         err = fixscript_get_array_elem(heap, script_handle->value, HANDLE_destroy, &func);
         if (err) {
            fixscript_error(heap, &error, err);
            fixscript_dump_value(heap, error, 1);
         }
         else {
            #ifdef __wasm__
               fixscript_allow_sync_call(heap);
            #endif
            fixscript_call(heap, func, 2, &error, script_handle->value, script_handle->script_heap_val);
            if (error.value) {
               fixscript_dump_value(heap, error, 1);
            }
         }
         if (script_handle->script_heap->handles == script_handle) {
            script_handle->script_heap->handles = script_handle->next;
         }
         if (script_handle->prev) {
            script_handle->prev->next = script_handle->next;
         }
         if (script_handle->next) {
            script_handle->next->prev = script_handle->prev;
         }
         free(script_handle);
         break;

      case HANDLE_OP_COMPARE:
         err = fixscript_get_array_elem(heap, script_handle->value, HANDLE_compare, &func);
         if (err) {
            fixscript_error(heap, &error, err);
            fixscript_dump_value(heap, error, 1);
            return (void *)1;
         }
         #ifdef __wasm__
            fixscript_allow_sync_call(heap);
         #endif
         ret = fixscript_call(heap, func, 3, &error, script_handle->value, script_handle->script_heap_val, ((ScriptHandle *)p2)->value);
         if (error.value) {
            fixscript_dump_value(heap, error, 1);
            return (void *)1;
         }
         return (void *)(intptr_t)ret.value;

      case HANDLE_OP_HASH:
         err = fixscript_get_array_elem(heap, script_handle->value, HANDLE_calc_hash, &func);
         if (err) {
            fixscript_error(heap, &error, err);
            fixscript_dump_value(heap, error, 1);
            return (void *)1;
         }
         #ifdef __wasm__
            fixscript_allow_sync_call(heap);
         #endif
         ret = fixscript_call(heap, func, 2, &error, script_handle->value, script_handle->script_heap_val);
         if (error.value) {
            fixscript_dump_value(heap, error, 1);
            return (void *)1;
         }
         return (void *)(intptr_t)ret.value;
         
      case HANDLE_OP_TO_STRING:
         err = fixscript_get_array_elem(heap, script_handle->value, HANDLE_to_string, &func);
         if (err) {
            fixscript_error(heap, &error, err);
            fixscript_dump_value(heap, error, 1);
            return NULL;
         }
         #ifdef __wasm__
            fixscript_allow_sync_call(heap);
         #endif
         ret = fixscript_call(heap, func, 2, &error, script_handle->value, script_handle->script_heap_val);
         if (error.value) {
            fixscript_dump_value(heap, error, 1);
            return NULL;
         }
         err = fixscript_get_string(heap, ret, 0, -1, &s, NULL);
         if (err) {
            fixscript_error(heap, &error, err);
            fixscript_dump_value(heap, error, 1);
            return NULL;
         }
         return s;

      case HANDLE_OP_MARK_REFS:
         err = fixscript_get_array_elem(heap, script_handle->value, HANDLE_mark_refs, &func);
         if (err) {
            fixscript_error(heap, &error, err);
            fixscript_dump_value(heap, error, 1);
            return NULL;
         }
         #ifdef __wasm__
            fixscript_allow_sync_call(heap);
         #endif
         fixscript_call(heap, func, 2, &error, script_handle->value, script_handle->script_heap_val);
         if (error.value) {
            fixscript_dump_value(heap, error, 1);
            return NULL;
         }
         return NULL;
   }

   return NULL;
}


typedef struct {
   ScriptHeap *script_heap;
   Heap *heap;
   Value load_func;
   Value load_data;
   Value heap_val;
} LoadScriptData;

static Script *load_script_func(Heap *heap, const char *fname, Value *error, void *data)
{
   LoadScriptData *lsd = data;
   Script *script;
   Value error2;
   const char *error_msg;
   char *buf;
   int err;

   buf = malloc(strlen(fname)+4+1);
   strcpy(buf, fname);
   strcat(buf, ".fix");
   script = fixscript_get(lsd->script_heap->heap, buf);
   if (script) {
      free(buf);
      return script;
   }

   #ifdef __wasm__
      fixscript_allow_sync_call(lsd->heap);
   #endif
   fixscript_call(lsd->heap, lsd->load_func, 3, error, lsd->load_data, lsd->heap_val, fixscript_create_string(lsd->heap, fname, -1));
   if (error->value) {
      free(buf);
      if (lsd->script_heap->hc) {
         script = lsd->script_heap->hc->load_func(lsd->script_heap->heap, fname, &error2, lsd->script_heap->hc->load_data);
         if (script) {
            *error = fixscript_int(0);
            return script;
         }
      }
      err = fixscript_clone_between(heap, lsd->heap, *error, error, NULL, NULL, NULL);
      if (err) {
         fixscript_error(heap, error, err);
      }
      return NULL;
   }

   script = fixscript_get(lsd->script_heap->heap, buf);
   free(buf);

   if (!script && lsd->script_heap->hc) {
      script = lsd->script_heap->hc->load_func(lsd->script_heap->heap, fname, &error2, lsd->script_heap->hc->load_data);
      if (!script) {
         error_msg = fixscript_get_compiler_error(lsd->script_heap->heap, *error);
         if (strchr(error_msg, '\n')) {
            err = fixscript_clone_between(heap, lsd->script_heap->heap, *error, error, NULL, NULL, NULL);
            if (err) {
               fixscript_error(heap, error, err);
            }
            else {
               *error = fixscript_create_error(heap, *error);
            }
         }
         else {
            *error = fixscript_create_error_string(heap, error_msg);
         }
         return NULL;
      }
      return script;
   }

   if (!script) {
      *error = fixscript_create_string(heap, "script wasn't loaded by callback function", -1);
   }
   return script;
}


static ScriptHeap *get_script_heap(Heap *heap, Value *error, Value value)
{
   ScriptHeap *script_heap;

   script_heap = fixscript_get_handle(heap, value, HANDLE_TYPE_HEAP, NULL);
   if (!script_heap) {
      *error = fixscript_create_error_string(heap, "invalid heap");
      return NULL;
   }
   if (!script_heap->heap) {
      *error = fixscript_create_error_string(heap, "heap is already destroyed");
      return NULL;
   }
   return script_heap;
}


static int get_script_value(Heap *heap, Value value, Value *out)
{
   Value values[2];
   int err;

   err = fixscript_get_array_range(heap, value, 0, 2, values);
   if (err) {
      return err;
   }

   out->value = values[0].value;
   out->is_array = values[1].value != 0;
   return FIXSCRIPT_SUCCESS;
}


static int create_script_value(Heap *heap, Value value, Value *out)
{
   Value values[2];

   *out = fixscript_create_array(heap, 2);
   if (!out->value) {
      return FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }

   values[0] = fixscript_int(value.value);
   values[1] = fixscript_int(value.is_array);
   return fixscript_set_array_range(heap, *out, 0, 2, values);
}


static Value script_heap_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   HeapCreateData *hc = data;
   ScriptHeap *script_heap;
   Value handle;

   script_heap = calloc(1, sizeof(ScriptHeap));
   if (!script_heap) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   if (hc) {
      script_heap->heap = hc->create_func(hc->create_data);
      script_heap->hc = hc;
   }
   else {
      script_heap->heap = fixscript_create_heap();
   }
   if (!script_heap->heap) {
      free(script_heap);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   script_heap->refcnt = 1;
   if (pthread_mutex_init(&script_heap->mutex, RECURSIVE_MUTEX_ATTR) != 0) {
      fixscript_free_heap(script_heap->heap);
      free(script_heap);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   handle = fixscript_create_value_handle(heap, HANDLE_TYPE_HEAP, script_heap, script_heap_handle_func);
   if (!handle.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return handle;
}


static Value script_heap_destroy(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   pthread_mutex_lock(&script_heap->mutex);
   fixscript_free_heap(script_heap->heap);
   script_heap->heap = NULL;
   pthread_mutex_unlock(&script_heap->mutex);

   return fixscript_int(0);
}


static Value script_heap_collect(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   fixscript_collect_heap(script_heap->heap);
   return fixscript_int(0);
}


static Value script_heap_get_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   long long size;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   size = (fixscript_heap_size(script_heap->heap) + 1023) >> 10;
   if (size > INT_MAX) {
      return fixscript_int(INT_MAX);
   }
   return fixscript_int((int)size);
}


static Value script_heap_adjust_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   fixscript_adjust_heap_size(script_heap->heap, params[1].value);
   return fixscript_int(0);
}


static Value script_heap_set_max_stack_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   fixscript_set_max_stack_size(script_heap->heap, params[1].value);
   return fixscript_int(0);
}


static Value script_heap_get_max_stack_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   return fixscript_int(fixscript_get_max_stack_size(script_heap->heap));
}


static Value script_heap_get_stack_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   return fixscript_int(fixscript_get_stack_size(script_heap->heap));
}


static Value script_heap_ref(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int do_ref = (int)(intptr_t)data;
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (do_ref) {
      fixscript_ref(script_heap->heap, value);
   }
   else {
      fixscript_unref(script_heap->heap, value);
   }
   return fixscript_int(0);
}


static Value script_heap_protected(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int set = (int)(intptr_t)data;
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (set) {
      fixscript_set_protected(script_heap->heap, value, params[2].value);
      return fixscript_int(0);
   }
   else {
      return fixscript_int(fixscript_is_protected(script_heap->heap, value));
   }
}


static Value script_heap_set_time_limit(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   fixscript_set_time_limit(script_heap->heap, params[1].value);
   return fixscript_int(0);
}


static Value script_heap_get_remaining_time(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   return fixscript_int(fixscript_get_remaining_time(script_heap->heap));
}


static Value script_heap_get_async(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   AsyncHeap *async_heap;
   Value handle;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   async_heap = calloc(1, sizeof(AsyncHeap));
   if (!async_heap) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   async_heap->script_heap = script_heap;
   (void)__sync_add_and_fetch(&script_heap->refcnt, 1);

   handle = fixscript_create_value_handle(heap, HANDLE_TYPE_ASYNC_HEAP, async_heap, async_heap_handle_func);
   if (!handle.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return handle;
}


static Value async_heap_stop_execution(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   AsyncHeap *async_heap;

   async_heap = fixscript_get_handle(heap, params[0], HANDLE_TYPE_ASYNC_HEAP, NULL);
   if (!async_heap) {
      *error = fixscript_create_error_string(heap, "invalid async heap");
      return fixscript_int(0);
   }

   pthread_mutex_lock(&async_heap->script_heap->mutex);
   if (async_heap->script_heap->heap) {
      fixscript_stop_execution(async_heap->script_heap->heap);
   }
   pthread_mutex_unlock(&async_heap->script_heap->mutex);
   return fixscript_int(0);
}


static Value script_heap_mark_ref(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   fixscript_mark_ref(script_heap->heap, value);
   return fixscript_int(0);
}


static Value script_heap_create_array(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   value = fixscript_create_array(script_heap->heap, params[1].value);
   if (!value.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = create_script_value(heap, value, &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return value;
}


static Value script_heap_set_array_length(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_set_array_length(script_heap->heap, value, params[2].value);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value script_heap_get_array_length(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err, len;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_get_array_length(script_heap->heap, value, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(len);
}


static Value script_heap_is_array(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(fixscript_is_array(script_heap->heap, value));
}


static Value script_heap_set_array_elem(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value array, value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &array);
   if (!err) {
      err = get_script_value(heap, params[3], &value);
   }
   if (!err) {
      err = fixscript_set_array_elem(script_heap->heap, array, params[2].value, value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value script_heap_get_array_elem(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value array, value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &array);
   if (!err) {
      err = fixscript_get_array_elem(script_heap->heap, array, params[2].value, &value);
   }
   if (!err) {
      err = create_script_value(heap, value, &value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return value;
}


static Value script_heap_append_array_elem(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value array, value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &array);
   if (!err) {
      err = get_script_value(heap, params[2], &value);
   }
   if (!err) {
      err = fixscript_append_array_elem(script_heap->heap, array, value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value script_heap_get_array_range(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value array, *values = NULL;
   int64_t size;
   int i, err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = get_script_value(heap, params[1], &array);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   size = (int64_t)params[3].value * sizeof(Value);
   if (size < 0 || size > INT_MAX) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   values = malloc((int)size);
   if (!values) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_array_range(script_heap->heap, array, params[2].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   for (i=0; i<params[3].value; i++) {
      err = create_script_value(heap, values[i], &values[i]);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }

   err = fixscript_set_array_range(heap, params[4], params[5].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

error:
   free(values);
   return fixscript_int(0);
}


static Value script_heap_set_array_range(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value array, *values = NULL;
   int64_t size;
   int i, err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = get_script_value(heap, params[1], &array);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   size = (int64_t)params[3].value * sizeof(Value);
   if (size < 0 || size > INT_MAX) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   values = malloc((int)size);
   if (!values) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_array_range(heap, params[4], params[5].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   for (i=0; i<params[3].value; i++) {
      err = get_script_value(heap, values[i], &values[i]);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }

   err = fixscript_set_array_range(script_heap->heap, array, params[2].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

error:
   free(values);
   return fixscript_int(0);
}


static Value script_heap_get_array_values(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value array, *values = NULL;
   int64_t size;
   int i, err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = get_script_value(heap, params[1], &array);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   size = (int64_t)params[3].value * sizeof(Value) * 2;
   if (size < 0 || size > INT_MAX) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   values = malloc((int)size);
   if (!values) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_array_range(script_heap->heap, array, params[2].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   for (i=params[3].value-1; i>=0; i--) {
      values[i*2+0] = fixscript_int(values[i].value);
      values[i*2+1] = fixscript_int(values[i].is_array);
   }

   err = fixscript_set_array_range(heap, params[4], params[5].value, params[3].value*2, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

error:
   free(values);
   return fixscript_int(0);
}


static Value script_heap_set_array_values(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value array, *values = NULL;
   int64_t size;
   int i, err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = get_script_value(heap, params[1], &array);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   size = (int64_t)params[3].value * sizeof(Value) * 2;
   if (size < 0 || size > INT_MAX) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   values = malloc((int)size);
   if (!values) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_array_range(heap, params[4], params[5].value, params[3].value*2, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   for (i=0; i<params[3].value; i++) {
      values[i].value = values[i*2+0].value;
      values[i].is_array = values[i*2+1].value != 0;
   }

   err = fixscript_set_array_range(script_heap->heap, array, params[2].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

error:
   free(values);
   return fixscript_int(0);
}


static Value script_heap_get_array_numbers(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value array, *values = NULL;
   int64_t size;
   int i, err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = get_script_value(heap, params[1], &array);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   size = (int64_t)params[3].value * sizeof(Value);
   if (size < 0 || size > INT_MAX) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   values = malloc((int)size);
   if (!values) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_array_range(script_heap->heap, array, params[2].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   for (i=0; i<params[3].value; i++) {
      if (!fixscript_is_int(values[i]) && !fixscript_is_float(values[i])) {
         values[i].is_array = 0;
      }
   }

   err = fixscript_set_array_range(heap, params[4], params[5].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

error:
   free(values);
   return fixscript_int(0);
}


static Value script_heap_set_array_numbers(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value array, *values = NULL;
   int64_t size;
   int i, err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = get_script_value(heap, params[1], &array);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   size = (int64_t)params[3].value * sizeof(Value);
   if (size < 0 || size > INT_MAX) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   values = malloc((int)size);
   if (!values) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_get_array_range(heap, params[4], params[5].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   for (i=0; i<params[3].value; i++) {
      if (!fixscript_is_int(values[i]) && !fixscript_is_float(values[i])) {
         values[i].is_array = 0;
      }
   }

   err = fixscript_set_array_range(script_heap->heap, array, params[2].value, params[3].value, values);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

error:
   free(values);
   return fixscript_int(0);
}


static Value script_heap_copy_array(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value dest, src;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &dest);
   if (!err) {
      err = get_script_value(heap, params[3], &src);
   }
   if (!err) {
      err = fixscript_copy_array(script_heap->heap, dest, params[2].value, src, params[4].value, params[5].value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value script_heap_create_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value *values = NULL, str, ret = fixscript_int(0);
   int64_t size;
   int off, len;
   int i, err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   if (num_params == 2) {
      off = 0;
      err = fixscript_get_array_length(heap, params[1], &len);
      if (err) {
         fixscript_error(heap, error, err);
         goto error;
      }
   }
   else {
      off = params[2].value;
      len = params[3].value;
   }

   size = (int64_t)len * sizeof(Value);
   if (size < 0 || size > INT_MAX) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   values = malloc((int)size);
   if (!values) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   str = fixscript_create_string(script_heap->heap, NULL, 0);
   if (!str.value) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      goto error;
   }

   err = fixscript_set_array_length(script_heap->heap, str, len);
   if (!err) {
      err = fixscript_get_array_range(heap, params[1], off, len, values);
   }
   if (!err) {
      for (i=0; i<len; i++) {
         values[i].is_array = 0;
      }
      err = fixscript_set_array_range(script_heap->heap, str, 0, len, values);
   }
   if (!err) {
      err = create_script_value(heap, str, &ret);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

error:
   free(values);
   return ret;
}


static Value script_heap_is_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(fixscript_is_string(script_heap->heap, value));
}


static Value script_heap_get_const_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err, off, len;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   if (num_params == 2) {
      off = 0;
      len = -1;
   }
   else {
      off = params[2].value;
      len = params[3].value;
      if (len < 0) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }
   }

   err = get_script_value(heap, params[1], &value);
   if (!err) {
      err = fixscript_get_const_string(script_heap->heap, value, off, len, &value);
   }
   if (!err) {
      err = create_script_value(heap, value, &value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return value;
}


static Value script_heap_is_const_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(fixscript_is_const_string(script_heap->heap, value));
}


static Value script_heap_create_hash(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value hash;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   hash = fixscript_create_hash(script_heap->heap);
   if (!hash.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = create_script_value(heap, hash, &hash);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return hash;
}


static Value script_heap_is_hash(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(fixscript_is_hash(script_heap->heap, value));
}


static Value script_heap_set_hash_elem(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value hash, key, value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &hash);
   if (!err) {
      err = get_script_value(heap, params[2], &key);
   }
   if (!err) {
      err = get_script_value(heap, params[3], &value);
   }
   if (!err) {
      err = fixscript_set_hash_elem(script_heap->heap, hash, key, value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value script_heap_get_hash_elem(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value hash, key, value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &hash);
   if (!err) {
      err = get_script_value(heap, params[2], &key);
   }
   if (!err) {
      err = fixscript_get_hash_elem(script_heap->heap, hash, key, &value);
      if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
         return fixscript_int(0);
      }
   }
   if (!err) {
      err = create_script_value(heap, value, &value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return value;
}


static Value script_heap_remove_hash_elem(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value hash, key, value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &hash);
   if (!err) {
      err = get_script_value(heap, params[2], &key);
   }
   if (!err) {
      err = fixscript_remove_hash_elem(script_heap->heap, hash, key, &value);
      if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
         return fixscript_int(0);
      }
   }
   if (!err) {
      err = create_script_value(heap, value, &value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return value;
}


static Value script_heap_clear_hash(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value hash;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &hash);
   if (!err) {
      err = fixscript_clear_hash(script_heap->heap, hash);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value script_heap_get_hash_entry(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value hash, key, value, args[2];
   NativeFunc nfunc;
   void *ndata;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   nfunc = fixscript_get_native_func(script_heap->heap, "hash_entry#2", &ndata);
   if (!nfunc) {
      *error = fixscript_create_error_string(heap, "hash_entry native function not found");
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &hash);
   if (!err) {
      args[0] = hash;
      args[1] = params[2];
      key = nfunc(script_heap->heap, &value, 2, args, ndata);
   }
   if (!err) {
      err = create_script_value(heap, key, &key);
   }
   if (!err) {
      err = create_script_value(heap, value, &value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   *error = value;
   return key;
}


static Value script_heap_create_handle(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   ScriptHandle *script_handle;
   Value handle;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   script_handle = calloc(1, sizeof(ScriptHandle));
   script_handle->heap = heap;
   script_handle->value = params[1];
   script_handle->script_heap = script_heap;
   script_handle->script_heap_val = params[0];

   script_handle->next = script_heap->handles;
   if (script_handle->next) {
      script_handle->next->prev = script_handle;
   }
   script_heap->handles = script_handle;

   handle = fixscript_create_value_handle(script_heap->heap, HANDLE_TYPE_HANDLE, script_handle, script_handle_handle_func);
   if (!handle.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = create_script_value(heap, handle, &handle);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return handle;
}


static Value script_heap_is_handle(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(fixscript_get_handle(script_heap->heap, value, -1, NULL) != NULL);
}


static Value script_heap_get_handle(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   ScriptHandle *script_handle;
   Value handle;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &handle);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   script_handle = fixscript_get_handle(script_heap->heap, handle, HANDLE_TYPE_HANDLE, NULL);
   if (!script_handle) {
      *error = fixscript_create_error_string(heap, "invalid handle");
      return fixscript_int(0);
   }

   return script_handle->value;
}


static Value script_heap_create_weak_ref(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value ref, value, container = fixscript_int(0), key = fixscript_int(0);
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (!err && num_params >= 3) {
      err = get_script_value(heap, params[2], &container);
   }
   if (!err && num_params >= 4) {
      err = get_script_value(heap, params[3], &key);
   }
   if (!err) {
      err = fixscript_create_weak_ref(script_heap->heap, value, num_params >= 3? &container : NULL, num_params >= 4? &key : NULL, &ref);
   }
   if (!err) {
      err = create_script_value(heap, ref, &ref);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ref;
}


static Value script_heap_get_weak_ref(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value ref, value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &ref);
   if (!err) {
      err = fixscript_get_weak_ref(script_heap->heap, ref, &value);
   }
   if (!err) {
      err = create_script_value(heap, value, &value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return value;
}


static Value script_heap_is_weak_ref(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(fixscript_is_weak_ref(script_heap->heap, value));
}


static Value script_heap_create_error(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   value = fixscript_create_error(script_heap->heap, value);
   if (!value.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = create_script_value(heap, value, &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return value;
}


static Value script_heap_dump_value(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   int newlines = 1;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (num_params == 3) {
      newlines = params[2].value;
   }

   err = fixscript_dump_value(script_heap->heap, value, newlines);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return fixscript_int(0);
}


static Value script_heap_to_string(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value, ret;
   char *s;
   int newlines = 0;
   int err, len;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (num_params == 3) {
      newlines = params[2].value;
   }

   err = fixscript_to_string(script_heap->heap, value, newlines, &s, &len);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   ret = fixscript_create_string(heap, s, len);
   free(s);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value script_heap_compare(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap1, *script_heap2;
   Value value1, value2;
   int err;

   script_heap1 = get_script_heap(heap, error, params[0]);
   if (!script_heap1) {
      return fixscript_int(0);
   }

   if (num_params == 4) {
      script_heap2 = get_script_heap(heap, error, params[2]);
      if (!script_heap2) {
         return fixscript_int(0);
      }
   }
   else {
      script_heap2 = script_heap1;
   }

   err = get_script_value(heap, params[1], &value1);
   if (!err) {
      err = get_script_value(heap, params[num_params-1], &value2);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   return fixscript_int(fixscript_compare_between(script_heap1->heap, value1, script_heap2->heap, value2));
}


static Value script_heap_clone(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   int deep = (int)(intptr_t)data;
   ScriptHeap *script_heap;
   Value value;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &value);
   if (!err) {
      err = fixscript_clone(script_heap->heap, value, deep, &value);
   }
   if (!err) {
      err = create_script_value(heap, value, &value);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return value;
}


static Value script_heap_clone_to(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   LoadScriptData lsd;
   LoadScriptFunc load_func = NULL;
   void *load_data = NULL;
   Value clone;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   if (num_params == 4 && params[2].value) {
      lsd.script_heap = script_heap;
      lsd.heap = heap;
      lsd.load_func = params[2];
      lsd.load_data = params[3];
      lsd.heap_val = params[0];
      if (lsd.load_func.value) {
         load_func = load_script_func;
         load_data = &lsd;
      }
   }

   err = fixscript_clone_between(script_heap->heap, heap, params[1], &clone, load_func, load_data, error);
   if (err) {
      if (error->value) {
         if (fixscript_clone_between(heap, script_heap->heap, *error, error, NULL, NULL, NULL) != FIXSCRIPT_SUCCESS) {
            *error = fixscript_int(0);
         }
      }
      if (error->value) {
         *error = fixscript_create_error(heap, *error);
      }
      else {
         fixscript_error(heap, error, err);
      }
      return fixscript_int(0);
   }

   err = create_script_value(heap, clone, &clone);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return clone;
}


static Value script_heap_clone_from(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value clone;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &params[1]);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_clone_between(heap, script_heap->heap, params[1], &clone, num_params == 3 && params[2].value? fixscript_resolve_existing : NULL, NULL, error);
   if (err) {
      if (error->value) {
         *error = fixscript_create_error(heap, *error);
      }
      else {
         fixscript_error(heap, error, err);
      }
      return fixscript_int(0);
   }

   return clone;
}


static Value script_heap_clone_between(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap_dest;
   ScriptHeap *script_heap_src;
   LoadScriptData lsd;
   LoadScriptFunc load_func = NULL;
   void *load_data = NULL;
   Value clone;
   int err;

   script_heap_dest = get_script_heap(heap, error, params[0]);
   if (!script_heap_dest) {
      return fixscript_int(0);
   }

   script_heap_src = get_script_heap(heap, error, params[1]);
   if (!script_heap_src) {
      return fixscript_int(0);
   }

   if (num_params == 5 && params[3].value) {
      lsd.script_heap = script_heap_dest;
      lsd.heap = heap;
      lsd.load_func = params[3];
      lsd.load_data = params[4];
      lsd.heap_val = params[0];
      if (lsd.load_func.value) {
         load_func = load_script_func;
         load_data = &lsd;
      }
   }

   err = get_script_value(heap, params[2], &params[2]);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_clone_between(script_heap_dest->heap, script_heap_src->heap, params[2], &clone, load_func, load_data, error);
   if (err) {
      if (error->value) {
         if (fixscript_clone_between(heap, script_heap_dest->heap, *error, error, NULL, NULL, NULL) != FIXSCRIPT_SUCCESS) {
            *error = fixscript_int(0);
         }
      }
      if (error->value) {
         *error = fixscript_create_error(heap, *error);
      }
      else {
         fixscript_error(heap, error, err);
      }
      return fixscript_int(0);
   }

   err = create_script_value(heap, clone, &clone);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return clone;
}


static Value script_heap_serialize(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value, array;
   char *buf = NULL;
   int64_t size;
   int err, len, pos=0;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[num_params-1], &value);
   if (!err) {
      err = fixscript_serialize_to_array(script_heap->heap, &buf, &len, value);
   }
   if (!err) {
      if (num_params == 3) {
         array = params[1];
         err = fixscript_get_array_length(heap, array, &pos);
         if (!err) {
            size = (int64_t)pos + len;
            if (size > INT_MAX) {
               err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
            }
         }
         if (!err) {
            err = fixscript_set_array_length(heap, array, pos + len);
         }
      }
      else {
         array = fixscript_create_array(heap, len);
         if (!array.value) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
   }
   if (!err) {
      err = fixscript_set_array_bytes(heap, array, pos, len, buf);
   }
   free(buf);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return array;
}


static Value script_heap_unserialize(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value value;
   char *buf;
   int err=0, off, off_ref, len;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   if (num_params == 2) {
      off = 0;
      err = fixscript_get_array_length(heap, params[1], &len);
   }
   else if (num_params == 3) {
      err = fixscript_get_array_elem(heap, params[2], 0, &value);
      if (!err) {
         off = value.value;
         err = fixscript_get_array_length(heap, params[1], &len);
         len -= off;
      }
   }
   else {
      off = params[2].value;
      len = params[3].value;
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_lock_array(heap, params[1], off, len, (void **)&buf, 1, ACCESS_READ_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   err = fixscript_unserialize_from_array(script_heap->heap, buf, num_params == 3? &off_ref : NULL, len, &value);
   if (!err && num_params == 3) {
      err = fixscript_set_array_elem(heap, params[2], 0, fixscript_int(off + off_ref));
   }
   if (!err) {
      err = create_script_value(heap, value, &value);
   }

   fixscript_unlock_array(heap, params[1], off, len, (void **)&buf, 1, ACCESS_READ_ONLY);
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return value;
}


static Value script_heap_load(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   char *script_name = NULL;
   char *src = NULL;
   const char *error_msg;
   Script *script;
   LoadScriptData lsd;
   int err, len;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &script_name, NULL);
   if (!err) {
      err = fixscript_get_array_length(heap, params[2], &len);
   }
   if (!err) {
      src = malloc(len+1);
      if (!src) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      err = fixscript_get_array_bytes(heap, params[2], 0, len, src);
      src[len] = 0;
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   lsd.script_heap = script_heap;
   lsd.heap = heap;
   lsd.load_func = params[3];
   lsd.load_data = params[4];
   lsd.heap_val = params[0];
   script = fixscript_load(script_heap->heap, src, script_name, error, lsd.load_func.value? load_script_func : NULL, &lsd);
   if (!script) {
      error_msg = fixscript_get_compiler_error(script_heap->heap, *error);
      if (strchr(error_msg, '\n')) {
         err = fixscript_clone_between(heap, script_heap->heap, *error, error, NULL, NULL, NULL);
         if (err) {
            fixscript_error(heap, error, err);
         }
         else {
            *error = fixscript_create_error(heap, *error);
         }
      }
      else {
         *error = fixscript_create_error_string(heap, error_msg);
      }
      goto error;
   }

error:
   free(script_name);
   free(src);
   return fixscript_int(0);
}


static Value script_heap_load_script(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   HeapCreateData *hc = data;
   ScriptHeap *script_heap;
   Script *script;
   char *name;
   const char *error_msg;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &name, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   script = hc->load_func(script_heap->heap, name, error, hc->load_data);
   free(name);
   if (!script) {
      error_msg = fixscript_get_compiler_error(script_heap->heap, *error);
      if (strchr(error_msg, '\n')) {
         err = fixscript_clone_between(heap, script_heap->heap, *error, error, NULL, NULL, NULL);
         if (err) {
            fixscript_error(heap, error, err);
         }
         else {
            *error = fixscript_create_error(heap, *error);
         }
      }
      else {
         *error = fixscript_create_error_string(heap, error_msg);
      }
   }
   return fixscript_int(0);
}


static Value script_heap_reload(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   char *script_name = NULL;
   char *src = NULL;
   Script *script;
   LoadScriptData lsd;
   const char *error_msg;
   int err, len;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &script_name, NULL);
   if (!err) {
      err = fixscript_get_array_length(heap, params[2], &len);
   }
   if (!err) {
      src = malloc(len+1);
      if (!src) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      err = fixscript_get_array_bytes(heap, params[2], 0, len, src);
      src[len] = 0;
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   lsd.script_heap = script_heap;
   lsd.heap = heap;
   lsd.load_func = params[3];
   lsd.load_data = params[4];
   lsd.heap_val = params[0];
   script = fixscript_reload(script_heap->heap, src, script_name, error, lsd.load_func.value? load_script_func : NULL, &lsd);
   if (!script) {
      error_msg = fixscript_get_compiler_error(script_heap->heap, *error);
      if (strchr(error_msg, '\n')) {
         err = fixscript_clone_between(heap, script_heap->heap, *error, error, NULL, NULL, NULL);
         if (err) {
            fixscript_error(heap, error, err);
         }
         else {
            *error = fixscript_create_error(heap, *error);
         }
      }
      else {
         *error = fixscript_create_error_string(heap, error_msg);
      }
      goto error;
   }

error:
   free(script_name);
   free(src);
   return fixscript_int(0);
}


static Value script_heap_is_loaded(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Script *script;
   char *name;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &name, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   script = fixscript_get(script_heap->heap, name);
   free(name);
   return fixscript_int(script != NULL);
}


static Value script_heap_get_function(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   char *script_name = NULL;
   char *func_name = NULL;
   Script *script;
   Value func, ret=fixscript_int(0);
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }
   
   err = fixscript_get_string(heap, params[1], 0, -1, &script_name, NULL);
   if (!err) {
      err = fixscript_get_string(heap, params[2], 0, -1, &func_name, NULL);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   script = fixscript_get(script_heap->heap, script_name);
   if (!script) {
      *error = fixscript_create_error_string(heap, "script not found");
      goto error;
   }

   func = fixscript_get_function(script_heap->heap, script, func_name);
   if (!func.value) {
      *error = fixscript_create_error_string(heap, "function not found");
      goto error;
   }

   err = create_script_value(heap, func, &func);
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   ret = func;

error:
   free(script_name);
   free(func_name);
   return ret;
}


static Value script_heap_get_function_info(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value func, values[3], ret;
   char *script_name;
   char *func_name;
   int err, num_args;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = get_script_value(heap, params[1], &func);
   if (!err) {
      err = fixscript_get_function_name(script_heap->heap, func, &script_name, &func_name, &num_args);
   }
   if (!err) {
      values[0] = fixscript_create_string(heap, script_name, -1);
      values[1] = fixscript_create_string(heap, func_name, -1);
      values[2] = fixscript_int(num_args);
      free(script_name);
      free(func_name);
      if (!values[0].value || !values[1].value) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      ret = fixscript_create_array(heap, 3);
      if (!ret.value) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      err = fixscript_set_array_range(heap, ret, 0, 3, values);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }
   return ret;
}


#ifdef __wasm__
typedef struct {
   Heap *heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
} HeapRunCont;

static void script_heap_run_cont(Heap *script_heap, Value result, Value error, void *data)
{
   HeapRunCont *cont = data;
   Heap *heap = cont->heap;
   Value script_values[2], array;
   ContinuationResultFunc cont_func;
   void *cont_data;
   int err;

   script_values[0] = result;
   script_values[1] = error;
   result = fixscript_int(0);
   error = fixscript_int(0);

   err = create_script_value(heap, script_values[0], &script_values[0]);
   if (!err) {
      err = create_script_value(heap, script_values[1], &script_values[1]);
   }
   if (!err) {
      array = fixscript_create_array(heap, 2);
      if (!array.value) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      err = fixscript_set_array_range(heap, array, 0, 2, script_values);
   }
   if (err) {
      fixscript_error(heap, &error, err);
      goto error;
   }

   result = array;

error:
   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);
   cont_func(heap, result, error, cont_data);
}
#endif


static Value script_heap_run(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   char *script_name = NULL;
   char *func_name = NULL;
   Value *args = NULL, func, ret=fixscript_int(0);
   Script *script;
   int64_t size;
   int i, err, len;
#ifdef __wasm__
   HeapRunCont *cont;
#else
   Value script_values[2], array;
#endif

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &script_name, NULL);
   if (!err) {
      err = fixscript_get_string(heap, params[2], 0, -1, &func_name, NULL);
   }
   if (!err) {
      err = fixscript_get_array_length(heap, params[3], &len);
   }
   if (!err) {
      size = len * sizeof(Value);
      if (size < 0 || size > INT_MAX) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      else {
         args = malloc((int)size);
         if (!args) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
   }
   if (!err) {
      err = fixscript_get_array_range(heap, params[3], 0, len, args);
   }
   if (!err) {
      for (i=0; i<len; i++) {
         err = get_script_value(heap, args[i], &args[i]);
         if (err) break;
      }
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   script = fixscript_get(script_heap->heap, script_name);
   if (!script) {
      *error = fixscript_create_error_string(heap, "script not found");
      goto error;
   }

   func = fixscript_get_function(script_heap->heap, script, func_name);
   if (!func.value) {
      *error = fixscript_create_error_string(heap, "function not found");
      goto error;
   }
   
#ifdef __wasm__
   cont = malloc(sizeof(HeapRunCont));
   cont->heap = heap;
   fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
   fixscript_call_async(script_heap->heap, func, len, args, script_heap_run_cont, cont);
#else
   script_values[0] = fixscript_call_args(script_heap->heap, func, len, &script_values[1], args);

   err = create_script_value(heap, script_values[0], &script_values[0]);
   if (!err) {
      err = create_script_value(heap, script_values[1], &script_values[1]);
   }
   if (!err) {
      array = fixscript_create_array(heap, 2);
      if (!array.value) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      err = fixscript_set_array_range(heap, array, 0, 2, script_values);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   ret = array;
#endif

error:
   free(script_name);
   free(func_name);
   free(args);
   return ret;
}


static Value script_heap_call(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   Value *args = NULL, func, ret = fixscript_int(0);
   int64_t size;
   int i, err, len;
#ifdef __wasm__
   HeapRunCont *cont;
#else
   Value script_values[2], array;
#endif

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      goto error;
   }

   err = get_script_value(heap, params[1], &func);
   if (!err) {
      err = fixscript_get_array_length(heap, params[2], &len);
   }
   if (!err) {
      size = len * sizeof(Value);
      if (size < 0 || size > INT_MAX) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
      else {
         args = malloc((int)size);
         if (!args) {
            err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
         }
      }
   }
   if (!err) {
      err = fixscript_get_array_range(heap, params[2], 0, len, args);
   }
   if (!err) {
      for (i=0; i<len; i++) {
         err = get_script_value(heap, args[i], &args[i]);
         if (err) break;
      }
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }
   
#ifdef __wasm__
   cont = malloc(sizeof(HeapRunCont));
   cont->heap = heap;
   fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
   fixscript_call_async(script_heap->heap, func, len, args, script_heap_run_cont, cont);
#else
   script_values[0] = fixscript_call_args(script_heap->heap, func, len, &script_values[1], args);

   err = create_script_value(heap, script_values[0], &script_values[0]);
   if (!err) {
      err = create_script_value(heap, script_values[1], &script_values[1]);
   }
   if (!err) {
      array = fixscript_create_array(heap, 2);
      if (!array.value) {
         err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
      }
   }
   if (!err) {
      err = fixscript_set_array_range(heap, array, 0, 2, script_values);
   }
   if (err) {
      fixscript_error(heap, error, err);
      goto error;
   }

   ret = array;
#endif

error:
   free(args);
   return ret;
}


typedef struct {
   Value script_heap_val;
   Heap *heap;
   Value func;
   Value data;
} ScriptNativeFunc;

#ifdef __wasm__
typedef struct {
   ScriptNativeFunc *snf;
   Heap *script_heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
} ScriptNativeFuncCont;

static void script_native_func_cont(Heap *host_heap, Value result, Value error, void *data)
{
   ScriptNativeFuncCont *cont = data;
   ScriptNativeFunc *snf = cont->snf;
   Heap *heap = cont->script_heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
   Value values[2];
   int err;

   if (error.value) {
      err = fixscript_clone_between(heap, snf->heap, error, &error, NULL, NULL, NULL);
      if (err) {
         error = fixscript_create_error_string(heap, "unknown error");
      }
      else {
         error = fixscript_create_error(heap, error);
      }
      result = fixscript_int(0);
      goto end;
   }
   if (!result.value) {
      result = fixscript_int(0);
      error = fixscript_int(0);
      goto end;
   }

   err = fixscript_get_array_range(snf->heap, result, 0, 2, values);
   if (err) {
      result = fixscript_error(heap, &error, err);
      goto end;
   }

   if (fixscript_is_int(values[0]) && fixscript_is_int(values[1])) {
      result.value = values[0].value;
      result.is_array = values[1].value != 0;
      error = fixscript_int(0);
      goto end;
   }

   if (!err) {
      err = get_script_value(snf->heap, values[0], &values[0]);
   }
   if (!err) {
      err = get_script_value(snf->heap, values[1], &values[1]);
   }
   if (err) {
      result = fixscript_error(heap, &error, err);
      goto end;
   }

   result = values[0];
   error = values[1];

end:
   cont_func = cont->cont_func;
   cont_data = cont->cont_data;
   free(cont);
   cont_func(heap, result, error, cont_data);
}
#endif

static Value script_native_func(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptNativeFunc *snf = data;
   Value args;
#ifdef __wasm__
   ScriptNativeFuncCont *cont;
#else
   Value ret, values[2], host_error;
#endif
   int i, err=0;

   args = fixscript_create_array(snf->heap, num_params);
   if (!args.value) {
      err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
   }
   if (!err) {
      for (i=0; i<num_params; i++) {
         err = create_script_value(snf->heap, params[i], &params[i]);
         if (err) break;
      }
   }
   if (!err) {
      err = fixscript_set_array_range(snf->heap, args, 0, num_params, params);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

#ifdef __wasm__
   cont = malloc(sizeof(ScriptNativeFuncCont));
   cont->snf = snf;
   cont->script_heap = heap;
   fixscript_suspend(heap, &cont->cont_func, &cont->cont_data);
   fixscript_call_async(snf->heap, snf->func, 3, (Value[]) { snf->data, snf->script_heap_val, args }, script_native_func_cont, cont);
   return fixscript_int(0);
#else
   ret = fixscript_call(snf->heap, snf->func, 3, &host_error, snf->data, snf->script_heap_val, args);
   if (host_error.value) {
      err = fixscript_clone_between(heap, snf->heap, host_error, &host_error, NULL, NULL, NULL);
      if (err) {
         *error = fixscript_create_error_string(heap, "unknown error");
      }
      else {
         *error = fixscript_create_error(heap, host_error);
      }
      return fixscript_int(0);
   }
   if (!ret.value) {
      return fixscript_int(0);
   }

   err = fixscript_get_array_range(snf->heap, ret, 0, 2, values);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   if (fixscript_is_int(values[0]) && fixscript_is_int(values[1])) {
      ret.value = values[0].value;
      ret.is_array = values[1].value != 0;
      return ret;
   }

   if (!err) {
      err = get_script_value(snf->heap, values[0], &values[0]);
   }
   if (!err) {
      err = get_script_value(snf->heap, values[1], &values[1]);
   }
   if (err) {
      return fixscript_error(heap, error, err);
   }

   *error = values[1];
   return values[0];
#endif
}

static Value script_heap_register_native_function(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ScriptHeap *script_heap;
   ScriptNativeFunc *snf;
   char *name;
   int err;

   script_heap = get_script_heap(heap, error, params[0]);
   if (!script_heap) {
      return fixscript_int(0);
   }

   err = fixscript_get_string(heap, params[1], 0, -1, &name, NULL);
   if (err) {
      return fixscript_error(heap, error, err);
   }

   snf = calloc(1, sizeof(ScriptNativeFunc));
   if (!snf) {
      free(name);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   snf->script_heap_val = params[0];
   snf->heap = heap;
   snf->func = params[2];
   snf->data = params[3];
   fixscript_ref(heap, snf->data);
   fixscript_register_cleanup(heap, free, snf);

   fixscript_register_native_func(script_heap->heap, name, script_native_func, snf);
   free(name);
   return fixscript_int(0);
}


static pthread_mutex_t *get_global_mutex()
{
   pthread_mutex_t *mutex, *new_mutex;

   mutex = (pthread_mutex_t *)global_mutex;
   if (mutex) {
      return mutex;
   }
   
   new_mutex = calloc(1, sizeof(pthread_mutex_t));
   if (!new_mutex) {
      return NULL;
   }
   if (pthread_mutex_init(new_mutex, NULL) != 0) {
      free(new_mutex);
      return NULL;
   }
   
   mutex = (pthread_mutex_t *)__sync_val_compare_and_swap(&global_mutex, NULL, new_mutex);
   if (mutex) {
      pthread_mutex_destroy(new_mutex);
      free(new_mutex);
   }
   else {
      mutex = new_mutex;
   }
   return mutex;
}


static int ensure_global_heap()
{
   if (!global_heap) {
      global_heap = fixscript_create_heap();
      if (!global_heap) {
         return 0;
      }
      global_hash = fixscript_create_hash(global_heap);
      fixscript_ref(global_heap, global_hash);
   }
   return 1;
}


static Value global_set(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   pthread_mutex_t *mutex;
   Value key, value;
   int err;
   
   mutex = get_global_mutex();
   if (!mutex) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   pthread_mutex_lock(mutex);
   if (!ensure_global_heap()) {
      pthread_mutex_unlock(mutex);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_clone_between(global_heap, heap, params[0], &key, NULL, NULL, NULL);
   if (!err) {
      err = fixscript_clone_between(global_heap, heap, params[1], &value, NULL, NULL, NULL);
   }
   if (!err) {
      err = fixscript_set_hash_elem(global_heap, global_hash, key, value);
   }
   if (err) {
      pthread_mutex_unlock(mutex);
      return fixscript_error(heap, error, err);
   }

   pthread_mutex_unlock(mutex);

   return fixscript_int(0);
}


static Value global_get(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   HeapCreateData *hc = data;
   pthread_mutex_t *mutex;
   Value key, value;
   int err;
   
   mutex = get_global_mutex();
   if (!mutex) {
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);

   if (!global_heap) {
      pthread_mutex_unlock(mutex);
      return fixscript_int(0);
   }

   err = fixscript_clone_between(global_heap, heap, params[0], &key, NULL, NULL, NULL);
   if (!err) {
      err = fixscript_get_hash_elem(global_heap, global_hash, key, &value);
      if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
         value = fixscript_int(0);
         err = FIXSCRIPT_SUCCESS;
      }
   }
   if (err) {
      pthread_mutex_unlock(mutex);
      return fixscript_error(heap, error, err);
   }

   err = fixscript_clone_between(heap, global_heap, value, &value, hc->load_func, hc->load_data, error);
   pthread_mutex_unlock(mutex);

   if (err) {
      if (error->value) {
         *error = fixscript_create_error(heap, *error);
      }
      else {
         fixscript_error(heap, error, err);
      }
      return fixscript_int(0);
   }

   return value;
}


static Value global_add(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   pthread_mutex_t *mutex;
   Value key, value;
   int err, prev_value=0;
   
   mutex = get_global_mutex();
   if (!mutex) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   pthread_mutex_lock(mutex);
   if (!ensure_global_heap()) {
      pthread_mutex_unlock(mutex);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   err = fixscript_clone_between(global_heap, heap, params[0], &key, NULL, NULL, NULL);
   if (!err) {
      err = fixscript_get_hash_elem(global_heap, global_hash, key, &value);
      if (err == FIXSCRIPT_ERR_KEY_NOT_FOUND) {
         value = fixscript_int(0);
         err = FIXSCRIPT_SUCCESS;
      }
   }
   if (!err) {
      prev_value = value.value;
      value = fixscript_int((uint32_t)value.value + (uint32_t)params[1].value);
      err = fixscript_set_hash_elem(global_heap, global_hash, key, value);
   }
   if (err) {
      pthread_mutex_unlock(mutex);
      return fixscript_error(heap, error, err);
   }

   pthread_mutex_unlock(mutex);

   return fixscript_int(prev_value);
}


static uint32_t *get_atomic_ptr32(Heap *heap, Value *error, Value array, int idx)
{
   uint32_t *ptr;
   int len, elem_size;

   ptr = fixscript_get_shared_array_data(heap, array, &len, &elem_size, NULL, -1, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid shared array reference");
      return NULL;
   }
   
   if (elem_size != 4) {
      *error = fixscript_create_error_string(heap, "element size must be 4 bytes");
      return NULL;
   }

   if (idx < 0 || idx >= len) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      return NULL;
   }

   return &ptr[idx];
}


static uint64_t *get_atomic_ptr64(Heap *heap, Value *error, Value array, int idx)
{
   uint64_t *ptr;
   int len, elem_size;

   if (idx & 1) {
      *error = fixscript_create_error_string(heap, "index must be aligned to 2");
      return NULL;
   }

   ptr = fixscript_get_shared_array_data(heap, array, &len, &elem_size, NULL, -1, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid shared array reference");
      return NULL;
   }

   if (((intptr_t)ptr) & 7) {
      *error = fixscript_create_error_string(heap, "shared array must be aligned to 8 bytes");
      return NULL;
   }
   
   if (elem_size != 4) {
      *error = fixscript_create_error_string(heap, "element size must be 4 bytes");
      return NULL;
   }

   idx >>= 1;
   len >>= 1;

   if (idx < 0 || idx >= len) {
      fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      return NULL;
   }

   return &ptr[idx];
}


static pthread_mutex_t *get_atomic_mutex(Heap *heap, Value *error, void *ptr)
{
   pthread_mutex_t *global;
   int i;
   
   if (!atomic_initialized) {
      global = get_global_mutex();
      if (!global) {
         if (heap) {
            fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
         return NULL;
      }

      pthread_mutex_lock(global);
      if (!atomic_initialized) {
         for (i=0; i<sizeof(atomic_mutex)/sizeof(pthread_mutex_t); i++) {
            if (pthread_mutex_init(&atomic_mutex[i], RECURSIVE_MUTEX_ATTR) != 0) {
               i--;
               while (i >= 0) {
                  pthread_mutex_destroy(&atomic_mutex[i--]);
               }
               if (heap) {
                  fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
               }
               pthread_mutex_unlock(global);
               return NULL;
            }
         }
      }
      pthread_mutex_unlock(global);

      __sync_val_compare_and_swap(&atomic_initialized, 0, 1);
   }

   return &atomic_mutex[hash_ptr(ptr) & (sizeof(atomic_mutex)/sizeof(pthread_mutex_t)-1)];
}


static Value atomic_get32(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   volatile uint32_t *ptr, value;

   ptr = get_atomic_ptr32(heap, error, params[0], params[1].value);
   if (!ptr) {
      return fixscript_int(0);
   }

#if defined(__i386__) || defined(__x86_64__)
   value = *ptr;
#else
   __sync_synchronize();
   value = *ptr;
   __sync_synchronize();
#endif

   return fixscript_int(value);
}


static Value atomic_get64(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   volatile uint64_t *ptr, value;

   ptr = get_atomic_ptr64(heap, error, params[0], params[1].value);
   if (!ptr) {
      return fixscript_int(0);
   }

#if INTPTR_MAX == INT64_MAX && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
   #if defined(__x86_64__)
      value = *ptr;
   #else
      __sync_synchronize();
      value = *ptr;
      __sync_synchronize();
   #endif
#else
   {
      pthread_mutex_t *mutex;
      
      mutex = get_atomic_mutex(heap, error, (uint64_t *)ptr);
      if (!mutex) {
         return fixscript_int(0);
      }

      pthread_mutex_lock(mutex);
      value = *ptr;
      pthread_mutex_unlock(mutex);
   }
#endif

   *error = fixscript_int((uint32_t)(value >> 32));
   return fixscript_int((uint32_t)value);
}


static Value atomic_set32(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   volatile uint32_t *ptr;

   ptr = get_atomic_ptr32(heap, error, params[0], params[1].value);
   if (!ptr) {
      return fixscript_int(0);
   }

   __sync_synchronize();
   *ptr = params[2].value;
   __sync_synchronize();

   return fixscript_int(0);
}


static Value atomic_set64(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   volatile uint64_t *ptr, value;

   ptr = get_atomic_ptr64(heap, error, params[0], params[1].value);
   if (!ptr) {
      return fixscript_int(0);
   }

   value = ((uint32_t)params[2].value) | (((uint64_t)(uint32_t)params[3].value) << 32);

#if INTPTR_MAX == INT64_MAX && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
   __sync_synchronize();
   *ptr = value;
   __sync_synchronize();
#else
   {
      pthread_mutex_t *mutex;
      
      mutex = get_atomic_mutex(heap, error, (uint64_t *)ptr);
      if (!mutex) {
         return fixscript_int(0);
      }

      pthread_mutex_lock(mutex);
      *ptr = value;
      pthread_mutex_unlock(mutex);
   }
#endif

   return fixscript_int(0);
}


static Value atomic_add32(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   volatile uint32_t *ptr, value;

   ptr = get_atomic_ptr32(heap, error, params[0], params[1].value);
   if (!ptr) {
      return fixscript_int(0);
   }

   value = __sync_fetch_and_add(ptr, params[2].value);
   return fixscript_int(value);
}


static Value atomic_add64(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   volatile uint64_t *ptr, value;

   ptr = get_atomic_ptr64(heap, error, params[0], params[1].value);
   if (!ptr) {
      return fixscript_int(0);
   }

   value = ((uint32_t)params[2].value) | (((uint64_t)(uint32_t)params[3].value) << 32);

#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8
   value = __sync_fetch_and_add(ptr, value);
#else
   {
      pthread_mutex_t *mutex;
      uint64_t prev_value;
      
      mutex = get_atomic_mutex(heap, error, (uint64_t *)ptr);
      if (!mutex) {
         return fixscript_int(0);
      }

      pthread_mutex_lock(mutex);
      prev_value = *ptr;
      *ptr = prev_value + value;
      value = prev_value;
      pthread_mutex_unlock(mutex);
   }
#endif

   *error = fixscript_int((uint32_t)(value >> 32));
   return fixscript_int((uint32_t)value);
}


static Value atomic_cas32(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   volatile uint32_t *ptr, value;

   ptr = get_atomic_ptr32(heap, error, params[0], params[1].value);
   if (!ptr) {
      return fixscript_int(0);
   }

   value = __sync_val_compare_and_swap(ptr, params[2].value, params[3].value);
   return fixscript_int(value);
}


static Value atomic_cas64(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   volatile uint64_t *ptr, expected, value;

   ptr = get_atomic_ptr64(heap, error, params[0], params[1].value);
   if (!ptr) {
      return fixscript_int(0);
   }

   expected = ((uint32_t)params[2].value) | (((uint64_t)(uint32_t)params[3].value) << 32);
   value = ((uint32_t)params[4].value) | (((uint64_t)(uint32_t)params[5].value) << 32);

#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8
   value = __sync_val_compare_and_swap(ptr, expected, value);
#else
   {
      pthread_mutex_t *mutex;
      uint64_t prev_value;
      
      mutex = get_atomic_mutex(heap, error, (uint64_t *)ptr);
      if (!mutex) {
         return fixscript_int(0);
      }

      pthread_mutex_lock(mutex);
      prev_value = *ptr;
      if (prev_value == expected) {
         *ptr = value;
      }
      value = prev_value;
      pthread_mutex_unlock(mutex);
   }
#endif

   *error = fixscript_int((uint32_t)(value >> 32));
   return fixscript_int((uint32_t)value);
}


static Value atomic_run(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   pthread_mutex_t *mutex;
   Value ret;
   uint8_t *ptr;
   int idx, len, elem_size;

   idx = params[1].value;

   if (params[0].value) {
      ptr = fixscript_get_shared_array_data(heap, params[0], &len, &elem_size, NULL, -1, NULL);
      if (!ptr) {
         *error = fixscript_create_error_string(heap, "invalid shared array reference");
         return fixscript_int(0);
      }

      if (idx < 0 || idx >= len) {
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_BOUNDS);
      }

      ptr += (uintptr_t)idx * (uintptr_t)elem_size;
   }
   else {
      ptr = (uint8_t *)(uintptr_t)(uint32_t)idx;
   }

   mutex = get_atomic_mutex(heap, error, ptr);
   if (!mutex) {
      return fixscript_int(0);
   }

   pthread_mutex_lock(mutex);
   #ifdef __wasm__
      fixscript_allow_sync_call(heap);
   #endif
   ret = fixscript_call(heap, params[2], 1, error, params[3]);
   pthread_mutex_unlock(mutex);

   return ret;
}


static void *barrier_handle_func(Heap *heap, int op, void *p1, void *p2)
{
   Barrier *barrier = p1;
   char buf[64];
#ifndef __wasm__
   int i;
#endif

   switch (op) {
      case HANDLE_OP_FREE:
         if (__sync_sub_and_fetch(&barrier->refcnt, 1) == 0) {
            pthread_mutex_destroy(&barrier->mutex);
            #ifdef __wasm__
               free(barrier->conts);
            #else
               for (i=0; i<barrier->max_waiting; i++) {
                  pthread_cond_destroy(&barrier->conds[i]);
               }
               free(barrier->conds);
            #endif
            free(barrier);
         }
         break;

      case HANDLE_OP_COPY:
         (void)__sync_add_and_fetch(&barrier->refcnt, 1);
         return barrier;

      case HANDLE_OP_COMPARE:
         return (void *)(intptr_t)(p1 == p2);

      case HANDLE_OP_HASH:
         return p1;
         
      case HANDLE_OP_TO_STRING:
         snprintf(buf, sizeof(buf), "barrier(%p,num_tasks=%d)", barrier, barrier->max_waiting+1);
         return strdup(buf);
   }

   return NULL;
}


static Value barrier_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Barrier *barrier;
   Value ret;
   int num_tasks = params[0].value;
#ifndef __wasm__
   int i;
#endif

   if (num_tasks <= 0) {
      *error = fixscript_create_error_string(heap, "number of tasks must be positive");
      return fixscript_int(0);
   }

   barrier = calloc(1, sizeof(Barrier));
   if (!barrier) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   barrier->refcnt = 1;
   barrier->max_waiting = num_tasks-1;

#ifdef __wasm__
   barrier->conts = calloc(barrier->max_waiting, sizeof(BarrierContinuation));
   if (!barrier->conts) {
      free(barrier);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
#else
   barrier->conds = calloc(barrier->max_waiting, sizeof(pthread_cond_t));
   if (!barrier->conds) {
      free(barrier);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   if (pthread_mutex_init(&barrier->mutex, NULL) != 0) {
      free(barrier->conds);
      free(barrier);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   for (i=0; i<barrier->max_waiting; i++) {
      if (pthread_cond_init(&barrier->conds[i], NULL) != 0) {
         while (--i >= 0) {
            pthread_cond_destroy(&barrier->conds[i]);
         }
         pthread_mutex_destroy(&barrier->mutex);
         free(barrier->conds);
         free(barrier);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
   }
#endif

   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_BARRIER, barrier, barrier_handle_func);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value barrier_wait(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Barrier *barrier;
#ifndef __wasm__
   uint32_t wait_value;
#endif
   int i, idx;

   barrier = fixscript_get_handle(heap, params[0], HANDLE_TYPE_BARRIER, NULL);
   if (!barrier) {
      *error = fixscript_create_error_string(heap, "invalid barrier handle");
      return fixscript_int(0);
   }

   pthread_mutex_lock(&barrier->mutex);
   if (barrier->num_waiting < 0 || barrier->num_waiting > barrier->max_waiting) {
      pthread_mutex_unlock(&barrier->mutex);
      *error = fixscript_create_error_string(heap, "memory corruption");
      return fixscript_int(0);
   }
   if (barrier->num_waiting == 0) {
      if (num_params == 2) {
         barrier->first_heap = heap;
         barrier->first_marker = params[1];
      }
      else {
         barrier->first_heap = NULL;
      }
   }
   else {
      if (num_params == 2) {
         if (!barrier->first_heap) {
            pthread_mutex_unlock(&barrier->mutex);
            *error = fixscript_create_error_string(heap, "marker mismatch");
            return fixscript_int(0);
         }
         if (!fixscript_compare_between(heap, params[1], barrier->first_heap, barrier->first_marker)) {
            pthread_mutex_unlock(&barrier->mutex);
            *error = fixscript_create_error_string(heap, "marker mismatch");
            return fixscript_int(0);
         }
      }
      else {
         if (barrier->first_heap) {
            pthread_mutex_unlock(&barrier->mutex);
            *error = fixscript_create_error_string(heap, "marker mismatch");
            return fixscript_int(0);
         }
      }
   }
   if (barrier->num_waiting == barrier->max_waiting) {
      barrier->num_waiting = 0;
      barrier->first_heap = NULL;
      barrier->counter_value++;
      #ifdef __wasm__
         for (i=0; i<barrier->max_waiting; i++) {
            wasm_sleep(0, barrier->conts[i].func, barrier->conts[i].data);
         }
      #else
         for (i=0; i<barrier->max_waiting; i++) {
            pthread_cond_signal(&barrier->conds[i]);
         }
      #endif
   }
   else {
      idx = barrier->num_waiting++;
      #ifdef __wasm__
         fixscript_suspend_void(heap, &barrier->conts[idx].func, &barrier->conts[idx].data);
      #else
         wait_value = barrier->counter_value;
         while (wait_value == barrier->counter_value) {
            pthread_cond_wait(&barrier->conds[idx], &barrier->mutex);
         }
      #endif
   }
   pthread_mutex_unlock(&barrier->mutex);
   return fixscript_int(0);
}


static void *channel_handler(Heap *heap, int op, void *p1, void *p2)
{
   Channel *channel = GET_PTR(p1);
   Heap *queue_heap;
   Value queue;
   const char *type;
   char buf[64];
   int flags;

   switch (op) {
      case HANDLE_OP_FREE:
         pthread_mutex_lock(&channel->mutex);
         if (fixscript_get_heap_data(heap, is_queue_heap_key)) {
            channel->weakcnt--;
            if (channel->weakcnt == 0 && channel->refcnt == 0) {
               pthread_mutex_unlock(&channel->mutex);
               pthread_mutex_destroy(&channel->mutex);
               pthread_cond_destroy(&channel->send_cond);
               pthread_cond_destroy(&channel->send_cond2);
               pthread_cond_destroy(&channel->receive_cond);
               free(channel);
               return NULL;
            }
         }
         else {
            if (--channel->refcnt == 0) {
               if (channel->size > 0) {
                  queue_heap = channel->queue_heap;
                  queue = channel->queue;
                  channel->queue_heap = NULL;
                  pthread_mutex_unlock(&channel->mutex);
                  fixscript_unref(queue_heap, queue);
                  fixscript_free_heap(queue_heap);
                  return NULL;
               }
            }
         }
         pthread_mutex_unlock(&channel->mutex);
         break;

      case HANDLE_OP_COPY:
         pthread_mutex_lock(&channel->mutex);
         if (channel->refcnt == 0) {
            pthread_mutex_unlock(&channel->mutex);
            return NULL;
         }
         channel->refcnt++;
         pthread_mutex_unlock(&channel->mutex);
         if (GET_FLAGS(p1) == CHANNEL_OWNED) {
            return WITH_FLAGS(p1, CHANNEL_BOTH);
         }
         return p1;

      case HANDLE_OP_COMPARE:
         return (void *)(channel == GET_PTR(p2));
         
      case HANDLE_OP_HASH:
         if (sizeof(void *) == 8) {
            uint64_t ptr = (uintptr_t)channel;
            return (void *)(uintptr_t)(ptr ^ (ptr >> 32));
         }
         return channel;

      case HANDLE_OP_TO_STRING:
         flags = GET_FLAGS(p1);
         if (flags == CHANNEL_SENDER) {
            type = "ChannelSender";
         }
         else if (flags == CHANNEL_RECEIVER) {
            type = "ChannelReceiver";
         }
         else {
            type = "Channel";
         }
         if (channel->size > 0) {
            snprintf(buf, sizeof(buf), "%s(%p,size=%d%s)", type, channel, channel->size, flags == CHANNEL_OWNED? ",owned" : "");
         }
         else {
            snprintf(buf, sizeof(buf), "%s(%p,sync%s)", type, channel, flags == CHANNEL_OWNED? ",owned" : "");
         }
         return strdup(buf);
   }

   return NULL;
}


static Value channel_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Channel *channel;
   Value ret;
   int err, size = 0;

   if (num_params == 1) {
      size = params[0].value;
   }
   if (size < 0) {
      *error = fixscript_create_error_string(heap, "size can't be negative");
      return fixscript_int(0);
   }

   #ifdef __wasm__
      if (size == 0) {
         size = 1; // TODO
      }
   #endif

   channel = calloc(1, sizeof(Channel));
   if (!channel) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   channel->refcnt = 1;
   channel->size = size;
   if (pthread_mutex_init(&channel->mutex, RECURSIVE_MUTEX_ATTR) != 0) {
      free(channel);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   if (pthread_cond_init(&channel->send_cond, NULL) != 0) {
      pthread_mutex_destroy(&channel->mutex);
      free(channel);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   if (pthread_cond_init(&channel->send_cond2, NULL) != 0) {
      pthread_cond_destroy(&channel->send_cond);
      pthread_mutex_destroy(&channel->mutex);
      free(channel);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   if (pthread_cond_init(&channel->receive_cond, NULL) != 0) {
      pthread_cond_destroy(&channel->send_cond2);
      pthread_cond_destroy(&channel->send_cond);
      pthread_mutex_destroy(&channel->mutex);
      free(channel);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   if (size > 0) {
      channel->queue_heap = fixscript_create_heap();
      if (!channel->queue_heap) {
         pthread_cond_destroy(&channel->send_cond2);
         pthread_cond_destroy(&channel->send_cond);
         pthread_cond_destroy(&channel->receive_cond);
         pthread_mutex_destroy(&channel->mutex);
         free(channel);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      err = fixscript_set_heap_data(channel->queue_heap, is_queue_heap_key, (void *)1, NULL);
      if (err) {
         fixscript_free_heap(channel->queue_heap);
         pthread_cond_destroy(&channel->send_cond2);
         pthread_cond_destroy(&channel->send_cond);
         pthread_cond_destroy(&channel->receive_cond);
         pthread_mutex_destroy(&channel->mutex);
         free(channel);
         return fixscript_error(heap, error, err);
      }
      channel->queue = fixscript_create_array(channel->queue_heap, 0);
      if (!channel->queue.value) {
         fixscript_free_heap(channel->queue_heap);
         pthread_cond_destroy(&channel->send_cond2);
         pthread_cond_destroy(&channel->send_cond);
         pthread_cond_destroy(&channel->receive_cond);
         pthread_mutex_destroy(&channel->mutex);
         free(channel);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }
      fixscript_ref(channel->queue_heap, channel->queue);
   }
   channel = WITH_FLAGS(channel, CHANNEL_OWNED);

   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_CHANNEL, channel, channel_handler);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


#ifdef __wasm__
static void channel_set_receive_notify(void *data);
#endif


static int notify_sets(Channel *channel)
{
   ChannelEntry *entry, **new_list;
   ChannelSet *set;
   int i, found, new_cap;

   for (entry = channel->notify_entries; entry; entry = entry->notify_next) {
      set = entry->set;
      pthread_mutex_lock(&set->mutex);
      found = 0;
      for (i=0; i<set->notify_cnt; i++) {
         if (set->notify_list[i] == entry) {
            found = 1;
            break;
         }
      }
      if (!found) {
         if (set->notify_cnt == set->notify_cap) {
            if (set->notify_cap >= 1<<27) {
               pthread_mutex_unlock(&set->mutex);
               return 0;
            }
            new_cap = set->notify_cap? set->notify_cap*2 : 4;
            new_list = realloc(set->notify_list, new_cap * sizeof(ChannelEntry *));
            if (!new_list) {
               pthread_mutex_unlock(&set->mutex);
               return 0;
            }
            set->notify_cap = new_cap;
            set->notify_list = new_list;
         }
         set->notify_list[set->notify_cnt++] = entry;
         pthread_cond_signal(&set->cond);

         #ifdef __wasm__
            if (set->cont_data) {
               channel_set_receive_notify(set->cont_data);
            }
         #endif
      }
      pthread_mutex_unlock(&set->mutex);
   }

   return 1;
}


static void unnotify_sets(Channel *channel)
{
   ChannelEntry *entry;
   ChannelSet *set;
   int i;

   for (entry = channel->notify_entries; entry; entry = entry->notify_next) {
      set = entry->set;
      pthread_mutex_lock(&set->mutex);
      for (i=0; i<set->notify_cnt; i++) {
         if (set->notify_list[i] == entry) {
            set->notify_list[i] = set->notify_list[--set->notify_cnt];
            break;
         }
      }
      pthread_mutex_unlock(&set->mutex);
   }
}


static void remove_notify(ChannelEntry *remove_entry)
{
   Channel *channel;
   ChannelEntry *entry, **prev;
   
   channel = remove_entry->channel;
   pthread_mutex_lock(&channel->mutex);
   prev = &channel->notify_entries;
   for (entry = *prev; entry; entry = entry->notify_next) {
      if (entry == remove_entry) {
         *prev = entry->notify_next;
         break;
      }
      prev = &entry->notify_next;
   }
   pthread_mutex_unlock(&channel->mutex);
}


#ifdef __wasm__
static void channel_wake_senders(Channel *channel)
{
   ChannelSender *s, **prev;

   if (channel->wasm_senders) {
      prev = &channel->wasm_senders;
      for (s = channel->wasm_senders; s; s = s->next) {
         if (!s->next) {
            *prev = NULL;
            wasm_sleep(0, s->wake_func, s);
            return;
         }
         prev = &s->next;
      }
   }
}

static void channel_wake_receivers(Channel *channel)
{
   ChannelReceiver *r, **prev;

   if (channel->wasm_receivers) {
      prev = &channel->wasm_receivers;
      for (r = channel->wasm_receivers; r; r = r->next) {
         if (!r->next) {
            *prev = NULL;
            wasm_sleep(0, r->wake_func, r);
            return;
         }
         prev = &r->next;
      }
   }

   notify_sets(channel);
}

static void channel_send_cont(void *data)
{
   ChannelSender *channel_sender = data;
   Heap *heap = channel_sender->heap;
   Value value = channel_sender->value;
   Channel *channel = channel_sender->channel;
   ContinuationResultFunc cont_func;
   void *cont_data;
   Value error;
   int err, len, had_timer=0;
   
   err = fixscript_get_array_length(channel->queue_heap, channel->queue, &len);
   if (!err && len >= channel->size) {
      channel_wake_receivers(channel);
      channel_sender->next = channel->wasm_senders;
      channel->wasm_senders = channel_sender;
      return;
   }

   if (channel_sender->cancel_timer != WASM_TIMER_NULL) {
      wasm_timer_stop(channel_sender->cancel_timer);
      had_timer = 1;
   }

   if (!err) {
      err = fixscript_clone_between(channel->queue_heap, heap, value, &value, NULL, NULL, NULL);
   }
   if (!err) {
      err = fixscript_append_array_elem(channel->queue_heap, channel->queue, value);
   }
   if (err) {
      cont_func = channel_sender->cont_func;
      cont_data = channel_sender->cont_data;
      free(channel_sender);
      fixscript_error(heap, &error, err);
      cont_func(heap, fixscript_int(0), error, cont_data);
      return;
   }

   channel_wake_receivers(channel);

   cont_func = channel_sender->cont_func;
   cont_data = channel_sender->cont_data;
   free(channel_sender);
   cont_func(heap, fixscript_int(had_timer? 1 : 0), fixscript_int(0), cont_data);
}

static void channel_send_cancel(void *data)
{
   ChannelSender *channel_sender = data;
   Channel *channel = channel_sender->channel;
   Heap *heap = channel_sender->heap;
   ChannelSender *s, **prev;
   ContinuationResultFunc cont_func;
   void *cont_data;

   if (channel->wasm_senders) {
      prev = &channel->wasm_senders;
      for (s = channel->wasm_senders; s; s = s->next) {
         if (s == channel_sender) {
            *prev = s->next;
            break;
         }
         prev = &s->next;
      }
   }

   cont_func = channel_sender->cont_func;
   cont_data = channel_sender->cont_data;
   free(channel_sender);
   cont_func(heap, fixscript_int(0), fixscript_int(0), cont_data);
}
#endif


static Value channel_send(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Channel *channel;
   Value value;
   int err, len, timeout = -1;
   void *ptr;
#ifdef __wasm__
   ChannelSender *channel_sender;
#endif
   uint64_t wait_until = 0;
   
   ptr = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid channel handle");
      return fixscript_int(0);
   }

   if (GET_FLAGS(ptr) == CHANNEL_RECEIVER) {
      *error = fixscript_create_error_string(heap, "can't send on receiver channel");
      return fixscript_int(0);
   }
   channel = GET_PTR(ptr);

   if (num_params == 3) {
      timeout = params[2].value;
   }

   pthread_mutex_lock(&channel->mutex);

   #ifndef __wasm__
      if (timeout > 0) {
         wait_until = get_time() + timeout;
      }
   #endif

   if (channel->size == 0) {
      while (channel->send_heap) {
         if (timeout < 0) {
            pthread_cond_wait(&channel->send_cond, &channel->mutex);
         }
         else {
            if (timeout > 0) {
               timeout = wait_until - get_time();
            }
            if (timeout <= 0 || pthread_cond_timedwait_relative(&channel->send_cond, &channel->mutex, timeout*1000000LL) == ETIMEDOUT) {
               pthread_mutex_unlock(&channel->mutex);
               return fixscript_int(0);
            }
         }
      }

      channel->send_heap = heap;
      channel->send_msg = params[1];
      channel->send_error = 0;
      pthread_cond_signal(&channel->receive_cond);

      if (!notify_sets(channel)) {
         channel->send_heap = NULL;
         pthread_mutex_unlock(&channel->mutex);
         return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
      }

      while (channel->send_heap == heap) {
         if (channel->send_error) {
            channel->send_heap = NULL;
            unnotify_sets(channel);
            err = channel->send_error;
            pthread_mutex_unlock(&channel->mutex);
            return fixscript_error(heap, error, err);
         }
         
         if (timeout < 0) {
            pthread_cond_wait(&channel->send_cond2, &channel->mutex);
         }
         else {
            if (timeout > 0) {
               timeout = wait_until - get_time();
            }
            if (timeout <= 0 || pthread_cond_timedwait_relative(&channel->send_cond2, &channel->mutex, timeout*1000000LL) == ETIMEDOUT) {
               if (channel->send_heap == heap) {
                  channel->send_heap = NULL;
                  unnotify_sets(channel);
               }
               pthread_mutex_unlock(&channel->mutex);
               return fixscript_int(0);
            }
         }
      }

      pthread_cond_signal(&channel->send_cond);
      pthread_mutex_unlock(&channel->mutex);
      return fixscript_int(num_params == 3? 1:0);
   }
   else {
      for (;;) {
         err = fixscript_get_array_length(channel->queue_heap, channel->queue, &len);
         if (err) {
            pthread_mutex_unlock(&channel->mutex);
            return fixscript_error(heap, error, err);
         }

         if (len < channel->size) {
            err = fixscript_clone_between(channel->queue_heap, heap, params[1], &value, NULL, NULL, NULL);
            if (!err) {
               err = fixscript_append_array_elem(channel->queue_heap, channel->queue, value);
            }
            pthread_cond_signal(&channel->receive_cond);
            #ifdef __wasm__
               channel_wake_receivers(channel);
            #endif
            if (!err) {
               if (!notify_sets(channel)) {
                  err = FIXSCRIPT_ERR_OUT_OF_MEMORY;
               }
            }
            pthread_mutex_unlock(&channel->mutex);
            if (err) {
               return fixscript_error(heap, error, err);
            }
            return fixscript_int(num_params == 3? 1:0);
         }

         #ifdef __wasm__
            if (timeout == 0) {
               return fixscript_int(0);
            }

            channel_sender = malloc(sizeof(ChannelSender));
            if (!channel_sender) {
               return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            }
            channel_sender->channel = channel;
            channel_sender->heap = heap;
            channel_sender->value = params[1];
            channel_sender->wake_func = channel_send_cont;
            fixscript_suspend(heap, &channel_sender->cont_func, &channel_sender->cont_data);
            channel_sender->cancel_timer = WASM_TIMER_NULL;
            channel_sender->next = channel->wasm_senders;
            channel->wasm_senders = channel_sender;

            if (timeout > 0) {
               channel_sender->cancel_timer = wasm_sleep(timeout, channel_send_cancel, channel_sender);
            }

            channel_wake_receivers(channel);
            return fixscript_int(0);
         #else
            if (timeout < 0) {
               pthread_cond_wait(&channel->send_cond, &channel->mutex);
            }
            else {
               if (timeout > 0) {
                  timeout = wait_until - get_time();
               }
               if (timeout <= 0 || pthread_cond_timedwait_relative(&channel->send_cond, &channel->mutex, timeout*1000000LL) == ETIMEDOUT) {
                  pthread_mutex_unlock(&channel->mutex);
                  return fixscript_int(0);
               }
            }
         #endif
      }
   }

   return fixscript_int(0);
}


#ifdef __wasm__
static void channel_set_suspend(Heap *heap, ContinuationResultFunc *func, void **data, void *csc_data);

static void channel_receive_cont(void *data)
{
   ChannelReceiver *channel_receiver = data;
   Channel *channel = channel_receiver->channel;
   Heap *heap = channel_receiver->heap;
   ContinuationResultFunc cont_func;
   void *cont_data;
   Value error = fixscript_int(0), value;
   int len, err;

   err = fixscript_get_array_length(channel->queue_heap, channel->queue, &len);
   if (!err && len == 0) {
      channel_wake_senders(channel);
      channel_receiver->next = channel->wasm_receivers;
      channel->wasm_receivers = channel_receiver;
      return;
   }

   if (channel_receiver->cancel_timer != WASM_TIMER_NULL) {
      wasm_timer_stop(channel_receiver->cancel_timer);
   }

   if (err) {
      cont_func = channel_receiver->cont_func;
      cont_data = channel_receiver->cont_data;
      free(channel_receiver);
      fixscript_error(heap, &error, err);
      cont_func(heap, fixscript_int(0), error, cont_data);
      return;
   }

   err = fixscript_get_array_elem(channel->queue_heap, channel->queue, 0, &value);
   if (!err) {
      err = fixscript_copy_array(channel->queue_heap, channel->queue, 0, channel->queue, 1, len-1);
   }
   if (!err) {
      err = fixscript_set_array_length(channel->queue_heap, channel->queue, len-1);
   }
   if (!err) {
      if (len-1 == 0) {
         unnotify_sets(channel);
      }
      err = fixscript_clone_between(heap, channel->queue_heap, value, &value, fixscript_resolve_existing, NULL, &error);
   }

   fixscript_collect_heap(channel->queue_heap);

   cont_func = channel_receiver->cont_func;
   cont_data = channel_receiver->cont_data;
   free(channel_receiver);

   if (err) {
      if (!error.value) {
         fixscript_error(heap, &error, err);
      }
      cont_func(heap, fixscript_int(0), error, cont_data);
      return;
   }

   cont_func(heap, value, fixscript_int(0), cont_data);
}

static void channel_receive_cancel(void *data)
{
   ChannelReceiver *channel_receiver = data;
   Channel *channel = channel_receiver->channel;
   Heap *heap = channel_receiver->heap;
   ChannelReceiver *r, **prev;
   ContinuationResultFunc cont_func;
   void *cont_data;
   Value timeout_value;

   prev = &channel->wasm_receivers;
   for (r = channel->wasm_receivers; r; r = r->next) {
      if (r == channel_receiver) {
         *prev = r->next;
         break;
      }
      prev = &r->next;
   }

   cont_func = channel_receiver->cont_func;
   cont_data = channel_receiver->cont_data;
   timeout_value = channel_receiver->timeout_value;
   free(channel_receiver);
   cont_func(heap, timeout_value, fixscript_int(0), cont_data);
}
#endif


static Value channel_receive(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Channel *channel;
   Value value;
   int err, len, timeout = -1;
   void *ptr;
#ifdef __wasm__
   ChannelReceiver *channel_receiver;
#endif
   uint64_t wait_until = 0;
   
   ptr = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid channel handle");
      return fixscript_int(0);
   }

   if (GET_FLAGS(ptr) == CHANNEL_SENDER) {
      *error = fixscript_create_error_string(heap, "can't receive on sender channel");
      return fixscript_int(0);
   }
   channel = GET_PTR(ptr);

   if (num_params == 3) {
      timeout = params[1].value;
   }

   pthread_mutex_lock(&channel->mutex);

   #ifndef __wasm__
      if (timeout > 0) {
         wait_until = get_time() + timeout;
      }
   #endif

   if (channel->size == 0) {
      for (;;) {
         while (!channel->send_heap || channel->send_error) {
            if (timeout < 0) {
               pthread_cond_wait(&channel->receive_cond, &channel->mutex);
            }
            else {
               if (timeout > 0) {
                  timeout = wait_until - get_time();
               }
               if (timeout <= 0 || pthread_cond_timedwait_relative(&channel->receive_cond, &channel->mutex, timeout*1000000LL) == ETIMEDOUT) {
                  pthread_mutex_unlock(&channel->mutex);
                  return params[2];
               }
            }
         }

         err = fixscript_clone_between(heap, channel->send_heap, channel->send_msg, &value, fixscript_resolve_existing, NULL, error);
         if (err == FIXSCRIPT_ERR_UNSERIALIZABLE_REF) {
            channel->send_error = err;
            pthread_cond_signal(&channel->send_cond2);
            continue;
         }
         break;
      }

      channel->send_heap = NULL;
      unnotify_sets(channel);
      pthread_cond_signal(&channel->send_cond2);
      pthread_mutex_unlock(&channel->mutex);

      if (err) {
         if (!error->value) {
            return fixscript_error(heap, error, err);
         }
         return fixscript_int(0);
      }
      return value;
   }
   else {
      for (;;) {
         err = fixscript_get_array_length(channel->queue_heap, channel->queue, &len);
         if (err) {
            pthread_mutex_unlock(&channel->mutex);
            return fixscript_error(heap, error, err);
         }

         if (len > 0) {
            err = fixscript_get_array_elem(channel->queue_heap, channel->queue, 0, &value);
            if (!err) {
               err = fixscript_copy_array(channel->queue_heap, channel->queue, 0, channel->queue, 1, len-1);
            }
            if (!err) {
               err = fixscript_set_array_length(channel->queue_heap, channel->queue, len-1);
            }
            #ifdef __wasm__
               channel_wake_senders(channel);
            #endif
            if (!err) {
               if (len-1 == 0) {
                  unnotify_sets(channel);
               }
               err = fixscript_clone_between(heap, channel->queue_heap, value, &value, fixscript_resolve_existing, NULL, error);
               if (err) {
                  pthread_cond_signal(&channel->send_cond);
                  pthread_mutex_unlock(&channel->mutex);
                  if (!error->value) {
                     return fixscript_error(heap, error, err);
                  }
                  return fixscript_int(0);
               }
               fixscript_collect_heap(channel->queue_heap);
            }
            pthread_cond_signal(&channel->send_cond);
            pthread_mutex_unlock(&channel->mutex);
            if (err) {
               return fixscript_error(heap, error, err);
            }
            return value;
         }

         #ifdef __wasm__
            if (timeout == 0) {
               return params[2];
            }

            channel_receiver = malloc(sizeof(ChannelReceiver));
            if (!channel_receiver) {
               return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
            }
            channel_receiver->channel = channel;
            channel_receiver->heap = heap;
            channel_receiver->wake_func = channel_receive_cont;
            if (data) {
               channel_set_suspend(heap, &channel_receiver->cont_func, &channel_receiver->cont_data, data);
            }
            else {
               fixscript_suspend(heap, &channel_receiver->cont_func, &channel_receiver->cont_data);
            }
            channel_receiver->cancel_timer = WASM_TIMER_NULL;
            channel_receiver->next = channel->wasm_receivers;
            channel->wasm_receivers = channel_receiver;

            channel_wake_senders(channel);

            if (timeout > 0) {
               channel_receiver->timeout_value = params[2];
               channel_receiver->cancel_timer = wasm_sleep(timeout, channel_receive_cancel, channel_receiver);
            }
            if (data) {
               return (Value) { 0, 2 };
            }
            return fixscript_int(0);
         #else
            if (timeout < 0) {
               pthread_cond_wait(&channel->receive_cond, &channel->mutex);
            }
            else {
               if (timeout > 0) {
                  timeout = wait_until - get_time();
               }
               if (timeout <= 0 || pthread_cond_timedwait_relative(&channel->receive_cond, &channel->mutex, timeout*1000000LL) == ETIMEDOUT) {
                  pthread_mutex_unlock(&channel->mutex);
                  return params[2];
               }
            }
         #endif
      }
   }
}


static Value channel_get_sender(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Channel *channel;
   Value ret;
   void *ptr;
   
   ptr = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid channel handle");
      return fixscript_int(0);
   }

   if (GET_FLAGS(ptr) == CHANNEL_SENDER) {
      return params[0];
   }

   if (GET_FLAGS(ptr) == CHANNEL_RECEIVER) {
      *error = fixscript_create_error_string(heap, "can't get sender channel from receiver channel");
      return fixscript_int(0);
   }

   channel = GET_PTR(ptr);

   pthread_mutex_lock(&channel->mutex);
   if (fixscript_get_heap_data(heap, is_queue_heap_key)) {
      channel->weakcnt++;
   }
   else {
      channel->refcnt++;
   }
   pthread_mutex_unlock(&channel->mutex);

   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_CHANNEL, WITH_FLAGS(channel, CHANNEL_SENDER), channel_handler);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value channel_get_receiver(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Channel *channel;
   Value ret;
   void *ptr;
   
   ptr = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid channel handle");
      return fixscript_int(0);
   }

   if (GET_FLAGS(ptr) == CHANNEL_RECEIVER) {
      return params[0];
   }

   if (GET_FLAGS(ptr) == CHANNEL_SENDER) {
      *error = fixscript_create_error_string(heap, "can't get receiver channel from sender channel");
      return fixscript_int(0);
   }

   channel = GET_PTR(ptr);

   pthread_mutex_lock(&channel->mutex);
   if (fixscript_get_heap_data(heap, is_queue_heap_key)) {
      channel->weakcnt++;
   }
   else {
      channel->refcnt++;
   }
   pthread_mutex_unlock(&channel->mutex);

   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_CHANNEL, WITH_FLAGS(channel, CHANNEL_RECEIVER), channel_handler);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value channel_get_shared_count(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Channel *channel;
   void *ptr;
   int64_t cnt;
   
   ptr = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid channel handle");
      return fixscript_int(0);
   }
   channel = GET_PTR(ptr);

   pthread_mutex_lock(&channel->mutex);
   cnt = (int64_t)channel->refcnt + (int64_t)channel->weakcnt;
   if (cnt > 0x7FFFFFFF) {
      cnt = 0x7FFFFFFF;
   }
   pthread_mutex_unlock(&channel->mutex);

   return fixscript_int(cnt);
}


static Value channel_set_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Channel *channel;
   void *ptr;
   int new_size;
   
   ptr = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid channel handle");
      return fixscript_int(0);
   }

   if (GET_FLAGS(ptr) != CHANNEL_OWNED) {
      *error = fixscript_create_error_string(heap, "can't change queue size on non-owned channel");
      return fixscript_int(0);
   }

   channel = GET_PTR(ptr);

   new_size = params[1].value;
   if (new_size < 1) {
      *error = fixscript_create_error_string(heap, "invalid size");
      return fixscript_int(0);
   }

   pthread_mutex_lock(&channel->mutex);
   if (channel->size == 0) {
      pthread_mutex_unlock(&channel->mutex);
      *error = fixscript_create_error_string(heap, "not asynchronous channel");
      return fixscript_int(0);
   }
   channel->size = new_size;
   pthread_mutex_unlock(&channel->mutex);

   return fixscript_int(0);
}


static Value channel_get_size(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   Channel *channel;
   void *ptr;
   int size;
   
   ptr = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid channel handle");
      return fixscript_int(0);
   }
   channel = GET_PTR(ptr);

   pthread_mutex_lock(&channel->mutex);
   size = channel->size;
   pthread_mutex_unlock(&channel->mutex);

   return fixscript_int(size);
}


static void *channel_set_handler(Heap *heap, int op, void *p1, void *p2)
{
   ChannelSet *set = p1;
   ChannelEntry *entry, *next;
   char buf[64];
   int i;

   switch (op) {
      case HANDLE_OP_FREE:
         pthread_mutex_destroy(&set->mutex);
         pthread_cond_destroy(&set->cond);
         for (i=0; i<set->entries_cap; i++) {
            entry = set->entries[i];
            while (entry) {
               next = entry->next;
               remove_notify(entry);
               free(entry);
               entry = next;
            }
         }
         free(set->entries);
         free(set->notify_list);
         #ifdef __wasm__
            free(set->cont_data);
         #endif
         free(set);
         break;

      case HANDLE_OP_TO_STRING:
         snprintf(buf, sizeof(buf), "ChannelSet(%p,count=%d)", set, set->entries_cnt);
         return strdup(buf);

      case HANDLE_OP_MARK_REFS:
         for (i=0; i<set->entries_cap; i++) {
            entry = set->entries[i];
            while (entry) {
               fixscript_mark_ref(heap, entry->channel_val);
               fixscript_mark_ref(heap, entry->key);
               entry = entry->next;
            }
         }
         break;
   }

   return NULL;
}


static Value channel_set_create(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ChannelSet *set;
   Value ret;

   set = calloc(1, sizeof(ChannelSet));
   if (!set) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   if (pthread_mutex_init(&set->mutex, RECURSIVE_MUTEX_ATTR) != 0) {
      free(set);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   if (pthread_cond_init(&set->cond, NULL) != 0) {
      pthread_mutex_destroy(&set->mutex);
      free(set);
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }

   set->entries_cap = 16;
   set->entries = calloc(set->entries_cap, sizeof(ChannelEntry *));
   if (!set->entries) {
      pthread_cond_destroy(&set->cond);
      pthread_mutex_destroy(&set->mutex);
      free(set);
   }

   ret = fixscript_create_value_handle(heap, HANDLE_TYPE_CHANNEL_SET, set, channel_set_handler);
   if (!ret.value) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   return ret;
}


static Value channel_set_add(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ChannelSet *set;
   Channel *channel;
   ChannelEntry *new_entry, *entry, *next, **prev, **new_entries;
   void *ptr;
   int i, err, len, idx;

   set = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL_SET, NULL);
   if (!set) {
      *error = fixscript_create_error_string(heap, "invalid channel set handle");
      return fixscript_int(0);
   }
   
   ptr = fixscript_get_handle(heap, params[1], HANDLE_TYPE_CHANNEL, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid channel handle");
      return fixscript_int(0);
   }

   if (GET_FLAGS(ptr) == CHANNEL_SENDER) {
      *error = fixscript_create_error_string(heap, "can't receive on sender channel");
      return fixscript_int(0);
   }
   channel = GET_PTR(ptr);

   new_entry = calloc(1, sizeof(ChannelEntry));
   if (!new_entry) {
      return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
   }
   new_entry->set = set;
   new_entry->channel = channel;
   new_entry->channel_val = params[1];
   new_entry->key = params[2];

   if (set->entries_cnt > set->entries_cap/2 && set->entries_cap < 4096) {
      new_entries = calloc(set->entries_cap*2, sizeof(ChannelEntry *));
      if (new_entries) {
         for (i=0; i<set->entries_cap; i++) {
            entry = set->entries[i];
            while (entry) {
               next = entry->next;
               idx = hash_ptr(entry->channel) & (set->entries_cap*2-1);
               entry->next = new_entries[idx];
               new_entries[idx] = entry;
               entry = next;
            }
         }
         free(set->entries);
         set->entries_cap *= 2;
         set->entries = new_entries;
      }
   }

   idx = hash_ptr(channel) & (set->entries_cap-1);
   prev = &set->entries[idx];
   for (entry = *prev; entry; entry = entry->next) {
      if (entry->channel == channel) {
         free(new_entry);
         *error = fixscript_create_error_string(heap, "channel is already added");
         return fixscript_int(0);
      }
      prev = &entry->next;
   }
   *prev = new_entry;
   set->entries_cnt++;

   pthread_mutex_lock(&channel->mutex);
   new_entry->notify_next = channel->notify_entries;
   channel->notify_entries = new_entry;
   if (channel->size == 0) {
      if (channel->send_heap && !channel->send_error) {
         if (!notify_sets(channel)) {
            pthread_mutex_unlock(&channel->mutex);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
      }
   }
   else {
      err = fixscript_get_array_length(channel->queue_heap, channel->queue, &len);
      if (err) {
         pthread_mutex_unlock(&channel->mutex);
         return fixscript_error(heap, error, err);
      }
      if (len > 0) {
         if (!notify_sets(channel)) {
            pthread_mutex_unlock(&channel->mutex);
            return fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         }
      }
   }
   pthread_mutex_unlock(&channel->mutex);

   return fixscript_int(0);
}


static Value channel_set_remove(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   ChannelSet *set;
   Channel *channel;
   ChannelEntry *entry, **prev;
   void *ptr;
   int idx;

   set = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL_SET, NULL);
   if (!set) {
      *error = fixscript_create_error_string(heap, "invalid channel set handle");
      return fixscript_int(0);
   }
   
   ptr = fixscript_get_handle(heap, params[1], HANDLE_TYPE_CHANNEL, NULL);
   if (!ptr) {
      *error = fixscript_create_error_string(heap, "invalid channel handle");
      return fixscript_int(0);
   }

   if (GET_FLAGS(ptr) == CHANNEL_SENDER) {
      *error = fixscript_create_error_string(heap, "can't receive on sender channel");
      return fixscript_int(0);
   }
   channel = GET_PTR(ptr);

   idx = hash_ptr(channel) & (set->entries_cap-1);
   prev = &set->entries[idx];
   for (entry = *prev; entry; entry = entry->next) {
      if (entry->channel == channel) {
         *prev = entry->next;
         set->entries_cnt--;
         remove_notify(entry);
         free(entry);
         return fixscript_int(0);
      }
      prev = &entry->next;
   }

   *error = fixscript_create_error_string(heap, "channel is not present");
   return fixscript_int(0);
}


#ifdef __wasm__
enum {
   CHANNEL_SET_RECEIVE_INIT,
   CHANNEL_SET_RECEIVE_ITERATE,
   CHANNEL_SET_RECEIVE_GOT_RESULT,
   CHANNEL_SET_RECEIVE_WAIT,
   CHANNEL_SET_RECEIVE_WAIT2,
   CHANNEL_SET_RECEIVE_TIMEOUT,
   CHANNEL_SET_RECEIVE_DONE
};

typedef struct {
   int state;
   Heap *heap;
   ChannelSet *set;
   Value error_key, timeout_key;
   int timeout;
   Value result_value, result_error;
   AsyncIntegration *ai;
   int idx;
   ChannelEntry *entry;
   wasm_timer_t wait_timer;
   int has_cont_func;
   ContinuationResultFunc cont_func;
   void *cont_data;
} ChannelSetCont;

static void channel_set_receive_timeout_cont(void *data);

static void channel_set_receive_cont(void *data)
{
   ChannelSetCont *csc = data;

   for (;;) {
      switch (csc->state) {
         case CHANNEL_SET_RECEIVE_INIT: {
            int process_async;

            csc->ai = fixscript_get_heap_data(csc->heap, async_integration_key);
            if (csc->ai) {
               process_async = 0;
               if (csc->ai->has_events) {
                  csc->ai->has_events = 0;
                  process_async = 1;
               }

               if (process_async) {
                  csc->ai->process_func(csc->heap);
               }
            }

            csc->state = CHANNEL_SET_RECEIVE_ITERATE;
            csc->idx = 0;
            continue;
         }

         case CHANNEL_SET_RECEIVE_ITERATE: {
            Value ret, error, receive_params[3];

            if (csc->idx < csc->set->notify_cnt) {
               csc->entry = csc->set->notify_list[csc->idx++];
               receive_params[0] = csc->entry->channel_val;
               receive_params[1] = fixscript_int(0);
               receive_params[2] = csc->timeout_key;
               error = fixscript_int(0);
               ret = channel_receive(csc->heap, &error, 3, receive_params, csc);
               csc->state = CHANNEL_SET_RECEIVE_GOT_RESULT;
               if (ret.is_array == 2) {
                  return;
               }
               csc->result_value = ret;
               csc->result_error = error;
               continue;
            }

            csc->state = CHANNEL_SET_RECEIVE_WAIT;
            continue;
         }

         case CHANNEL_SET_RECEIVE_GOT_RESULT: {
            Value ret, error;

            ret = csc->result_value;
            error = csc->result_error;

            if (error.value) {
               csc->result_value = csc->error_key;
               csc->result_error = fixscript_int(0);
               csc->state = CHANNEL_SET_RECEIVE_DONE;
               continue;
            }
            if (ret.value != csc->timeout_key.value || ret.is_array != csc->timeout_key.is_array) {
               csc->result_value = csc->entry->key;
               csc->result_error = ret;
               csc->state = CHANNEL_SET_RECEIVE_DONE;
               continue;
            }

            csc->state = CHANNEL_SET_RECEIVE_ITERATE;
            continue;
         }

         case CHANNEL_SET_RECEIVE_WAIT: {
            if (csc->timeout == 0) {
               csc->state = CHANNEL_SET_RECEIVE_TIMEOUT;
               continue;
            }

            if (csc->timeout > 0) {
               csc->wait_timer = wasm_sleep(csc->timeout, channel_set_receive_timeout_cont, csc);
            }

            csc->state = CHANNEL_SET_RECEIVE_WAIT2;
            if (!csc->has_cont_func) {
               csc->has_cont_func = 1;
               fixscript_suspend(csc->heap, &csc->cont_func, &csc->cont_data);
            }
            return;
         }

         case CHANNEL_SET_RECEIVE_WAIT2: {
            abort();
            return;
         }
         
         case CHANNEL_SET_RECEIVE_TIMEOUT: {
            csc->result_value = csc->timeout_key;
            csc->result_error = fixscript_int(0);
            csc->state = CHANNEL_SET_RECEIVE_DONE;
            continue;
         }

         case CHANNEL_SET_RECEIVE_DONE: {
            if (csc->has_cont_func) {
               csc->cont_func(csc->heap, csc->result_value, csc->result_error, csc->cont_data);
            }
            return;
         }
      }
   }
}

static void channel_set_receive_result_cont(Heap *heap, Value result, Value error, void *data)
{
   ChannelSetCont *csc = data;

   csc->result_value = result;
   csc->result_error = error;
   channel_set_receive_cont(csc);
}

static void channel_set_suspend(Heap *heap, ContinuationResultFunc *func, void **data, void *csc_data)
{
   ChannelSetCont *csc = csc_data;
   
   if (!csc->has_cont_func) {
      csc->has_cont_func = 1;
      fixscript_suspend(heap, &csc->cont_func, &csc->cont_data);
   }
   *func = channel_set_receive_result_cont;
   *data = csc;
}

static void channel_set_receive_timeout_cont(void *data)
{
   ChannelSetCont *csc = data;

   if (csc->state == CHANNEL_SET_RECEIVE_WAIT2) {
      csc->state = CHANNEL_SET_RECEIVE_TIMEOUT;
      csc->wait_timer = WASM_TIMER_NULL;
      channel_set_receive_cont(csc);
   }
}

static void channel_set_receive_notify(void *data)
{
   ChannelSetCont *csc = data;

   if (csc->state == CHANNEL_SET_RECEIVE_WAIT2) {
      if (csc->wait_timer != WASM_TIMER_NULL) {
         wasm_timer_stop(csc->wait_timer);
         csc->wait_timer = WASM_TIMER_NULL;
      }
      csc->state = CHANNEL_SET_RECEIVE_ITERATE;
      csc->idx = 0;
      wasm_sleep(0, channel_set_receive_cont, csc);
   }
}
#endif


static Value channel_set_receive(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
#ifdef __wasm__
   ChannelSet *set;
   ChannelSetCont *csc;
   Value error_key = params[1];
   Value ret;

   set = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL_SET, NULL);
   if (!set) {
      *error = fixscript_create_error_string(heap, "invalid channel set handle");
      return error_key;
   }

   if (set->cont_data) {
      csc = set->cont_data;
      if (csc->state != CHANNEL_SET_RECEIVE_DONE) {
         *error = fixscript_create_error_string(heap, "internal error: invalid state");
         return error_key;
      }
   }
   else {
      csc = malloc(sizeof(ChannelSetCont));
      if (!csc) {
         fixscript_error(heap, error, FIXSCRIPT_ERR_OUT_OF_MEMORY);
         return error_key;
      }
      set->cont_data = csc;
   }

   csc->state = CHANNEL_SET_RECEIVE_INIT;
   csc->heap = heap;
   csc->set = set;
   csc->error_key = error_key;
   csc->timeout = params[2].value;
   csc->timeout_key = params[3];
   csc->wait_timer = WASM_TIMER_NULL;
   csc->has_cont_func = 0;

   channel_set_receive_cont(csc);
   if (csc->has_cont_func) {
      return fixscript_int(0);
   }

   ret = csc->result_value;
   *error = csc->result_error;
   return ret;
#else
   ChannelSet *set;
   ChannelEntry *entry;
   AsyncIntegration *ai;
   Value error_key, timeout_key, ret, receive_params[3], return_value = fixscript_int(0);
   uint64_t wait_until = 0;
   int i, timeout = -1, process_async;

   error_key = params[1];
   timeout = params[2].value;
   timeout_key = params[3];
  
   set = fixscript_get_handle(heap, params[0], HANDLE_TYPE_CHANNEL_SET, NULL);
   if (!set) {
      *error = fixscript_create_error_string(heap, "invalid channel set handle");
      return error_key;
   }

   if (timeout > 0) {
      wait_until = get_time() + timeout;
   }

   ai = fixscript_get_heap_data(heap, async_integration_key);
   if (ai) {
      pthread_mutex_lock(&ai->mutex);
      ai->wait_mutex = &set->mutex;
      ai->wait_cond = &set->cond;
      pthread_mutex_unlock(&ai->mutex);
   }

   pthread_mutex_lock(&set->mutex);
   for (;;) {
      if (ai) {
         process_async = 0;
         if (ai->has_events) {
            ai->has_events = 0;
            process_async = 1;
         }

         if (process_async) {
            ai->process_func(heap);
         }
      }

      for (i=0; i<set->notify_cnt; i++) {
         entry = set->notify_list[i];
         pthread_mutex_unlock(&set->mutex);
         receive_params[0] = entry->channel_val;
         receive_params[1] = fixscript_int(0);
         receive_params[2] = timeout_key;
         ret = channel_receive(heap, error, 3, receive_params, NULL);
         if (error->value) {
            return_value = error_key;
            goto end;
         }
         if (ret.value != timeout_key.value || ret.is_array != timeout_key.is_array) {
            *error = ret;
            return_value = entry->key;
            goto end;
         }
         pthread_mutex_lock(&set->mutex);
      }

      if (timeout < 0) {
         pthread_cond_wait(&set->cond, &set->mutex);
      }
      else {
         if (timeout > 0) {
            timeout = wait_until - get_time();
         }
         if (timeout <= 0 || pthread_cond_timedwait_relative(&set->cond, &set->mutex, timeout*1000000LL) == ETIMEDOUT) {
            pthread_mutex_unlock(&set->mutex);
            return_value = timeout_key;
            goto end;
         }
      }
   }

end:
   if (ai) {
      pthread_mutex_lock(&ai->mutex);
      while (ai->sending_signal) {
         pthread_cond_wait(&ai->cond, &ai->mutex);
      }
      ai->wait_mutex = NULL;
      ai->wait_cond = NULL;
      pthread_mutex_unlock(&ai->mutex);
   }
   return return_value;
#endif
}


static Value dispatcher_get_time(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   return fixscript_int((uint32_t)get_time());
}


static void channel_set_loop_notify(void *data)
{
   AsyncIntegration *ai = data;
   pthread_mutex_t *mutex = NULL;
   pthread_cond_t *cond = NULL;

   pthread_mutex_lock(&ai->mutex);
   if (ai->wait_mutex) {
      mutex = ai->wait_mutex;
      cond = ai->wait_cond;
      ai->sending_signal = 1;
   }
   else {
      ai->has_events = 1;
   }
   pthread_mutex_unlock(&ai->mutex);

   if (mutex) {
      pthread_mutex_lock(mutex);
      ai->has_events = 1;
      pthread_cond_signal(cond);
      pthread_mutex_unlock(mutex);

      pthread_mutex_lock(&ai->mutex);
      ai->sending_signal = 0;
      pthread_cond_signal(&ai->cond);
      pthread_mutex_unlock(&ai->mutex);
   }
}


static Value dispatcher_integrate_async(Heap *heap, Value *error, int num_params, Value *params, void *data)
{
   AsyncIntegration *ai;

   ai = fixscript_get_heap_data(heap, async_integration_key);
   if (!ai) {
      *error = fixscript_create_error_string(heap, "async integration is not available");
      return fixscript_int(0);
   }

   if (ai->active) {
      *error = fixscript_create_error_string(heap, "async integration is already active");
      return fixscript_int(0);
   }

   ai->active = 1;
   ai->integrate_func(heap, channel_set_loop_notify, ai);
   return fixscript_int(0);
}


void fixtask_register_functions(Heap *heap, HeapCreateFunc create_func, void *create_data, LoadScriptFunc load_func, void *load_data)
{
   HeapCreateData *hc;
#ifndef _WIN32
   pthread_mutex_t *mutex;
#endif

   fixscript_register_handle_types(&handles_offset, NUM_HANDLE_TYPES);
   fixscript_register_heap_key(&heap_create_data_key);
   fixscript_register_heap_key(&cur_task_key);
   fixscript_register_heap_key(&compute_tasks_key);
   fixscript_register_heap_key(&is_queue_heap_key);
   fixscript_register_heap_key(&parent_heap_key);
   fixscript_register_heap_key(&async_integration_key);

   hc = malloc(sizeof(HeapCreateData));
   hc->create_func = create_func;
   hc->create_data = create_data;
   hc->load_func = load_func;
   hc->load_data = load_data;
   fixscript_set_heap_data(heap, heap_create_data_key, hc, free);

#ifndef _WIN32
   mutex = get_global_mutex();
   pthread_mutex_lock(mutex);
   if (!recursive_mutex_attr_initialized) {
      pthread_mutexattr_init(&recursive_mutex_attr);
      pthread_mutexattr_settype(&recursive_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
      recursive_mutex_attr_initialized = 1;
   }
   pthread_mutex_unlock(mutex);
#endif

   fixscript_register_native_func(heap, "task_create#2", task_create, hc);
   fixscript_register_native_func(heap, "task_create#3", task_create, hc);
   fixscript_register_native_func(heap, "task_create#4", task_create, hc);
   fixscript_register_native_func(heap, "task_get#0", task_get, NULL);
   fixscript_register_native_func(heap, "task_send#1", task_send, NULL);
   fixscript_register_native_func(heap, "task_send#2", task_send, NULL);
   fixscript_register_native_func(heap, "task_receive#0", task_receive, (void *)0);
   fixscript_register_native_func(heap, "task_receive#1", task_receive, (void *)0);
   fixscript_register_native_func(heap, "task_receive_wait#1", task_receive, (void *)1);
   fixscript_register_native_func(heap, "task_receive_wait#2", task_receive, (void *)1);
   fixscript_register_native_func(heap, "task_sleep#1", sleep_func, NULL);

   fixscript_register_native_func(heap, "compute_task_run#2", compute_task_run, hc);
   fixscript_register_native_func(heap, "compute_task_run#4", compute_task_run, hc);
   fixscript_register_native_func(heap, "compute_task_check_finished#0", compute_task_check_finished, NULL);
   fixscript_register_native_func(heap, "compute_task_finish_all#0", compute_task_finish_all, NULL);
   fixscript_register_native_func(heap, "compute_task_get_core_count#0", compute_task_get_core_count, NULL);
   fixscript_register_native_func(heap, "compute_task_run_parallel#4", compute_task_run_parallel, hc);
   fixscript_register_native_func(heap, "compute_task_run_parallel#5", compute_task_run_parallel, hc);

   fixscript_register_native_func(heap, "parent_ref_length#1", parent_ref_length, NULL);
   fixscript_register_native_func(heap, "parent_ref_array_get#2", parent_ref_array_get, NULL);
   fixscript_register_native_func(heap, "parent_ref_is_array#1", parent_ref_is_check, (void *)CHECK_ARRAY);
   fixscript_register_native_func(heap, "parent_ref_is_string#1", parent_ref_is_check, (void *)CHECK_STRING);
   fixscript_register_native_func(heap, "parent_ref_is_hash#1", parent_ref_is_check, (void *)CHECK_HASH);
   fixscript_register_native_func(heap, "parent_ref_is_shared#1", parent_ref_is_check, (void *)CHECK_SHARED);
   fixscript_register_native_func(heap, "parent_ref_is_funcref#1", parent_ref_is_check, (void *)CHECK_FUNCREF);
   fixscript_register_native_func(heap, "parent_ref_is_weakref#1", parent_ref_is_check, (void *)CHECK_WEAKREF);
   fixscript_register_native_func(heap, "parent_ref_is_handle#1", parent_ref_is_check, (void *)CHECK_HANDLE);
   fixscript_register_native_func(heap, "parent_ref_get#1", parent_ref_get, (void *)1);
   fixscript_register_native_func(heap, "parent_ref_clone#1", parent_ref_get, (void *)0);
   fixscript_register_native_func(heap, "parent_ref_get_shared_count#1", parent_ref_get_shared_count, NULL);
   fixscript_register_native_func(heap, "parent_ref_get_element_size#1", parent_ref_get_element_size, NULL);
   fixscript_register_native_func(heap, "parent_ref_copy_to#5", parent_ref_copy_to, NULL);
   fixscript_register_native_func(heap, "parent_ref_extract#3", parent_ref_extract, NULL);
   fixscript_register_native_func(heap, "parent_ref_weakref_get#1", parent_ref_weakref_get, NULL);
   fixscript_register_native_func(heap, "parent_ref_hash_get#3", parent_ref_hash_get, NULL);
   fixscript_register_native_func(heap, "parent_ref_hash_contains#2", parent_ref_hash_contains, NULL);
   fixscript_register_native_func(heap, "parent_ref_to_string#1", parent_ref_to_string, NULL);
   fixscript_register_native_func(heap, "parent_ref_to_string#2", parent_ref_to_string, NULL);

   fixscript_register_native_func(heap, "heap_create#0", script_heap_create, NULL);
   fixscript_register_native_func(heap, "heap_create_full#0", script_heap_create, hc);
   fixscript_register_native_func(heap, "heap_destroy#1", script_heap_destroy, NULL);
   fixscript_register_native_func(heap, "heap_collect#1", script_heap_collect, NULL);
   fixscript_register_native_func(heap, "heap_get_size#1", script_heap_get_size, NULL);
   fixscript_register_native_func(heap, "heap_adjust_size#2", script_heap_adjust_size, NULL);
   fixscript_register_native_func(heap, "heap_set_max_stack_size#2", script_heap_set_max_stack_size, NULL);
   fixscript_register_native_func(heap, "heap_get_max_stack_size#1", script_heap_get_max_stack_size, NULL);
   fixscript_register_native_func(heap, "heap_get_stack_size#1", script_heap_get_stack_size, NULL);
   fixscript_register_native_func(heap, "heap_ref#2", script_heap_ref, (void *)1);
   fixscript_register_native_func(heap, "heap_unref#2", script_heap_ref, (void *)0);
   fixscript_register_native_func(heap, "heap_set_protected#3", script_heap_protected, (void *)1);
   fixscript_register_native_func(heap, "heap_is_protected#2", script_heap_protected, (void *)0);
   fixscript_register_native_func(heap, "heap_set_time_limit#2", script_heap_set_time_limit, NULL);
   fixscript_register_native_func(heap, "heap_get_remaining_time#1", script_heap_get_remaining_time, NULL);
   fixscript_register_native_func(heap, "heap_get_async#1", script_heap_get_async, NULL);
   fixscript_register_native_func(heap, "async_heap_stop_execution#1", async_heap_stop_execution, NULL);
   fixscript_register_native_func(heap, "heap_mark_ref#2", script_heap_mark_ref, NULL);
   fixscript_register_native_func(heap, "heap_create_array#2", script_heap_create_array, NULL);
   fixscript_register_native_func(heap, "heap_set_array_length#3", script_heap_set_array_length, NULL);
   fixscript_register_native_func(heap, "heap_get_array_length#2", script_heap_get_array_length, NULL);
   fixscript_register_native_func(heap, "heap_is_array#2", script_heap_is_array, NULL);
   fixscript_register_native_func(heap, "heap_set_array_elem#4", script_heap_set_array_elem, NULL);
   fixscript_register_native_func(heap, "heap_get_array_elem#3", script_heap_get_array_elem, NULL);
   fixscript_register_native_func(heap, "heap_append_array_elem#3", script_heap_append_array_elem, NULL);
   fixscript_register_native_func(heap, "heap_get_array_range#6", script_heap_get_array_range, NULL);
   fixscript_register_native_func(heap, "heap_set_array_range#6", script_heap_set_array_range, NULL);
   fixscript_register_native_func(heap, "heap_get_array_values#6", script_heap_get_array_values, NULL);
   fixscript_register_native_func(heap, "heap_set_array_values#6", script_heap_set_array_values, NULL);
   fixscript_register_native_func(heap, "heap_get_array_numbers#6", script_heap_get_array_numbers, NULL);
   fixscript_register_native_func(heap, "heap_set_array_numbers#6", script_heap_set_array_numbers, NULL);
   fixscript_register_native_func(heap, "heap_copy_array#6", script_heap_copy_array, NULL);
   fixscript_register_native_func(heap, "heap_create_string#2", script_heap_create_string, NULL);
   fixscript_register_native_func(heap, "heap_create_string#4", script_heap_create_string, NULL);
   fixscript_register_native_func(heap, "heap_is_string#2", script_heap_is_string, NULL);
   fixscript_register_native_func(heap, "heap_get_const_string#2", script_heap_get_const_string, NULL);
   fixscript_register_native_func(heap, "heap_get_const_string#4", script_heap_get_const_string, NULL);
   fixscript_register_native_func(heap, "heap_is_const_string#2", script_heap_is_const_string, NULL);
   fixscript_register_native_func(heap, "heap_create_hash#1", script_heap_create_hash, NULL);
   fixscript_register_native_func(heap, "heap_is_hash#2", script_heap_is_hash, NULL);
   fixscript_register_native_func(heap, "heap_set_hash_elem#4", script_heap_set_hash_elem, NULL);
   fixscript_register_native_func(heap, "heap_get_hash_elem#3", script_heap_get_hash_elem, NULL);
   fixscript_register_native_func(heap, "heap_remove_hash_elem#3", script_heap_remove_hash_elem, NULL);
   fixscript_register_native_func(heap, "heap_clear_hash#2", script_heap_clear_hash, NULL);
   fixscript_register_native_func(heap, "heap_get_hash_entry#3", script_heap_get_hash_entry, NULL);
   fixscript_register_native_func(heap, "heap_create_handle#2", script_heap_create_handle, NULL);
   fixscript_register_native_func(heap, "heap_is_handle#2", script_heap_is_handle, NULL);
   fixscript_register_native_func(heap, "heap_get_handle#2", script_heap_get_handle, NULL);
   fixscript_register_native_func(heap, "heap_create_weak_ref#2", script_heap_create_weak_ref, NULL);
   fixscript_register_native_func(heap, "heap_create_weak_ref#3", script_heap_create_weak_ref, NULL);
   fixscript_register_native_func(heap, "heap_create_weak_ref#4", script_heap_create_weak_ref, NULL);
   fixscript_register_native_func(heap, "heap_get_weak_ref#2", script_heap_get_weak_ref, NULL);
   fixscript_register_native_func(heap, "heap_is_weak_ref#2", script_heap_is_weak_ref, NULL);
   fixscript_register_native_func(heap, "heap_create_error#2", script_heap_create_error, NULL);
   fixscript_register_native_func(heap, "heap_dump_value#2", script_heap_dump_value, NULL);
   fixscript_register_native_func(heap, "heap_dump_value#3", script_heap_dump_value, NULL);
   fixscript_register_native_func(heap, "heap_to_string#2", script_heap_to_string, NULL);
   fixscript_register_native_func(heap, "heap_to_string#3", script_heap_to_string, NULL);
   fixscript_register_native_func(heap, "heap_compare#3", script_heap_compare, NULL);
   fixscript_register_native_func(heap, "heap_compare_between#4", script_heap_compare, NULL);
   fixscript_register_native_func(heap, "heap_clone#2", script_heap_clone, (void *)0);
   fixscript_register_native_func(heap, "heap_clone_deep#2", script_heap_clone, (void *)1);
   fixscript_register_native_func(heap, "heap_clone_to#2", script_heap_clone_to, NULL);
   fixscript_register_native_func(heap, "heap_clone_to#4", script_heap_clone_to, NULL);
   fixscript_register_native_func(heap, "heap_clone_from#2", script_heap_clone_from, NULL);
   fixscript_register_native_func(heap, "heap_clone_from#3", script_heap_clone_from, NULL);
   fixscript_register_native_func(heap, "heap_clone_between#3", script_heap_clone_between, NULL);
   fixscript_register_native_func(heap, "heap_clone_between#5", script_heap_clone_between, NULL);
   fixscript_register_native_func(heap, "heap_serialize#2", script_heap_serialize, NULL);
   fixscript_register_native_func(heap, "heap_serialize#3", script_heap_serialize, NULL);
   fixscript_register_native_func(heap, "heap_unserialize#2", script_heap_unserialize, NULL);
   fixscript_register_native_func(heap, "heap_unserialize#3", script_heap_unserialize, NULL);
   fixscript_register_native_func(heap, "heap_unserialize#4", script_heap_unserialize, NULL);
   fixscript_register_native_func(heap, "heap_load#5", script_heap_load, NULL);
   fixscript_register_native_func(heap, "heap_load_script#2", script_heap_load_script, hc);
   fixscript_register_native_func(heap, "heap_reload#5", script_heap_reload, NULL);
   fixscript_register_native_func(heap, "heap_is_loaded#2", script_heap_is_loaded, NULL);
   fixscript_register_native_func(heap, "heap_get_function#3", script_heap_get_function, NULL);
   fixscript_register_native_func(heap, "heap_get_function_info#2", script_heap_get_function_info, NULL);
   fixscript_register_native_func(heap, "heap_run#4", script_heap_run, NULL);
   fixscript_register_native_func(heap, "heap_call#3", script_heap_call, NULL);
   fixscript_register_native_func(heap, "heap_register_native_function#4", script_heap_register_native_function, NULL);

   fixscript_register_native_func(heap, "global_set#2", global_set, NULL);
   fixscript_register_native_func(heap, "global_get#1", global_get, hc);
   fixscript_register_native_func(heap, "global_add#2", global_add, hc);

   fixscript_register_native_func(heap, "atomic_get32#2", atomic_get32, NULL);
   fixscript_register_native_func(heap, "atomic_get64#2", atomic_get64, NULL);
   fixscript_register_native_func(heap, "atomic_set32#3", atomic_set32, NULL);
   fixscript_register_native_func(heap, "atomic_set64#4", atomic_set64, NULL);
   fixscript_register_native_func(heap, "atomic_add32#3", atomic_add32, NULL);
   fixscript_register_native_func(heap, "atomic_add64#4", atomic_add64, NULL);
   fixscript_register_native_func(heap, "atomic_cas32#4", atomic_cas32, NULL);
   fixscript_register_native_func(heap, "atomic_cas64#6", atomic_cas64, NULL);
   fixscript_register_native_func(heap, "atomic_run#4", atomic_run, NULL);

   fixscript_register_native_func(heap, "barrier_create#1", barrier_create, NULL);
   fixscript_register_native_func(heap, "barrier_wait#1", barrier_wait, NULL);
   fixscript_register_native_func(heap, "barrier_wait#2", barrier_wait, NULL);

   fixscript_register_native_func(heap, "channel_create#0", channel_create, NULL);
   fixscript_register_native_func(heap, "channel_create#1", channel_create, NULL);
   fixscript_register_native_func(heap, "channel_send#2", channel_send, NULL);
   fixscript_register_native_func(heap, "channel_send#3", channel_send, NULL);
   fixscript_register_native_func(heap, "channel_receive#1", channel_receive, NULL);
   fixscript_register_native_func(heap, "channel_receive#3", channel_receive, NULL);
   fixscript_register_native_func(heap, "channel_get_sender#1", channel_get_sender, NULL);
   fixscript_register_native_func(heap, "channel_get_receiver#1", channel_get_receiver, NULL);
   fixscript_register_native_func(heap, "channel_get_shared_count#1", channel_get_shared_count, NULL);
   fixscript_register_native_func(heap, "channel_set_size#2", channel_set_size, NULL);
   fixscript_register_native_func(heap, "channel_get_size#1", channel_get_size, NULL);

   fixscript_register_native_func(heap, "channel_set_create#0", channel_set_create, NULL);
   fixscript_register_native_func(heap, "channel_set_add#3", channel_set_add, NULL);
   fixscript_register_native_func(heap, "channel_set_remove#2", channel_set_remove, NULL);
   fixscript_register_native_func(heap, "channel_set_receive#4", channel_set_receive, NULL);

   fixscript_register_native_func(heap, "dispatcher_get_time#0", dispatcher_get_time, NULL);
   fixscript_register_native_func(heap, "dispatcher_integrate_async#0", dispatcher_integrate_async, NULL);
}


void fixtask_get_script_load_function(Heap *heap, LoadScriptFunc *load_func, void **load_data)
{
   HeapCreateData *hc = fixscript_get_heap_data(heap, heap_create_data_key);

   if (hc) {
      *load_func = hc->load_func;
      *load_data = hc->load_data;
   }
   else {
      *load_func = NULL;
      *load_data = NULL;
   }
}


int fixtask_get_core_count(Heap *heap)
{
#ifdef __wasm__
   return 1;
#else
   ComputeTasks *tasks;
   int num_cores;

   tasks = get_compute_tasks(heap, fixscript_get_heap_data(heap, heap_create_data_key));
   if (tasks) {
      return tasks->num_cores;
   }
   
   num_cores = get_number_of_cores();
   if (num_cores < 1) num_cores = 1;
   return num_cores;
#endif
}


void fixtask_run_on_compute_threads(Heap *heap, Value *error, ComputeHeapRunFunc func, void *data)
{
#ifdef __wasm__
   func(heap, 0, data);
#else
   ComputeTasks *tasks;
   ComputeHeap *cheap;
   int i, num_inactive;

   if (error) {
      *error = fixscript_int(0);
   }
   compute_task_finish_all(heap, error, 0, NULL, NULL);
   if (error->value) {
      return;
   }

   tasks = get_compute_tasks(heap, fixscript_get_heap_data(heap, heap_create_data_key));
   if (!tasks) {
      *error = fixscript_create_error_string(heap, "can't initialize compute threads");
      return;
   }

   pthread_mutex_lock(&tasks->mutex);
   for (i=0; i<tasks->num_cores; i++) {
      cheap = tasks->inactive_heaps;
      tasks->inactive_heaps = cheap->inactive_next;

      cheap->run_func = func;
      cheap->run_data = data;
      cheap->core_id = i;

      cheap->active_next = tasks->active_heaps;
      tasks->active_heaps = cheap;
      pthread_cond_signal(&tasks->conds[i]);
   }
   pthread_mutex_unlock(&tasks->mutex);

   pthread_mutex_lock(&tasks->mutex);
   for (;;) {
      num_inactive = 0;
      cheap = tasks->inactive_heaps;
      while (cheap) {
         num_inactive++;
         cheap = cheap->inactive_next;
      }
      if (num_inactive == tasks->num_heaps) break;

      pthread_cond_wait(&tasks->cond, &tasks->mutex);
   }
   pthread_mutex_unlock(&tasks->mutex);
#endif
}


void *fixtask_get_atomic_mutex(void *ptr)
{
   return get_atomic_mutex(NULL, NULL, ptr);
}


static void free_async_integration(void *ptr)
{
   AsyncIntegration *ai = ptr;

   if (ai->active) {
      // TODO: currently we need to leak memory (and IO helper thread) on task destruction as the integration is permanent once activated
      return;
   }

   pthread_cond_destroy(&ai->cond);
   pthread_mutex_destroy(&ai->mutex);
   free(ai);
}


void __fixtask_integrate_io_event_loop(Heap *heap, void (*integrate_func)(Heap *, void (*)(void *), void *), void (*process_func)(Heap *))
{
   AsyncIntegration *ai;

   if (fixscript_get_heap_data(heap, async_integration_key)) {
      return;
   }

   ai = calloc(1, sizeof(AsyncIntegration));
   ai->integrate_func = integrate_func;
   ai->process_func = process_func;
   pthread_mutex_init(&ai->mutex, RECURSIVE_MUTEX_ATTR); // TODO: provide way to handle errors
   pthread_cond_init(&ai->cond, NULL);
   fixscript_set_heap_data(heap, async_integration_key, ai, free_async_integration);
}
