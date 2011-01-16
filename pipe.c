/**
 * The MIT License
 * Copyright (c) 2011 Clark Gaebel <cg.wowus.cg@gmail.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "pipe.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef min
#define min(a, b) ((a) <= (b) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) ((a) >= (b) ? (a) : (b))
#endif

// Runs a memcpy, then returns the end of the range copied.
// Has identical functionality as mempcpy, but is portable.
static inline void* offset_memcpy(void* restrict dest, const void* restrict src, size_t n)
{
    return (char*)memcpy(dest, src, n) + n;
}

/*
 * Pipe implementation overview
 * =================================
 *
 * A pipe is implemented as a circular buffer. There are two special cases for
 * this structure: nowrap and wrap.
 *
 * Nowrap:
 *
 *     buffer          begin               end                 bufend
 *       [               >==================>                    ]
 *
 * In this case, the data storage is contiguous, allowing easy access. This is
 * the simplest case.
 *
 * Wrap:
 *
 *     buffer        end                 begin                 bufend
 *       [============>                    >=====================]
 *
 * In this case, the data storage is split up, wrapping around to the beginning
 * of the buffer when it hits bufend. Hackery must be done in this case to
 * ensure the structure is maintained and data can be easily copied in/out.
 *
 * Invariants:
 *
 * The invariants of a pipe are documented in the check_invariants function,
 * and double-checked frequently in debug builds. This helps restore sanity when
 * making modifications, but may slow down calls. It's best to disable the
 * checks in release builds.
 *
 * Thread-safety:
 *
 * pipe_t has been designed with high threading workloads foremost in my mind.
 * Its initial purpose was to serve as a task queue, with multiple threads
 * feeding data in (from disk, network, etc) and multiple threads reading it
 * and processing it in parallel. This created the need for a fully re-entrant,
 * lightweight, accomodating data structure.
 *
 * No fancy threading tricks are used thus far. It's just a simple mutex
 * guarding the pipe, with a condition variable to signal when we have new
 * elements so the blocking consumers can get them. If you modify the pipe,
 * lock the mutex. Keep it locked for as short as possible.
 *
 * Complexity:
 *
 * Pushing and popping must run in O(n) where n is the number of elements being
 * inserted/removed. It must also run in O(1) with respect to the number of
 * elements in the pipe.
 *
 * Efficiency:
 *
 * Asserts are used liberally, and many of them, when inlined, can be turned
 * into no-ops. Therefore, it is recommended that you compile with -O1 in
 * debug builds as the pipe can easily become a bottleneck.
 */
struct pipe {
    size_t elem_size;  // The size of each element. This is read-only and
                       // therefore does not need to be locked to read.
    size_t elem_count; // The number of elements currently in the pipe.
    size_t capacity;   // The maximum number of elements the buffer can hold
                       // before a reallocation.
    size_t min_cap;    // The smallest sane capacity before the buffer refuses
                       // to shrink because it would just end up growing again.
    size_t max_cap;    // The maximum capacity (unlimited if zero) of the pipe
                       // before push requests are blocked. This is read-only
                       // and therefore does not need to be locked to read.

    char * buffer,     // The internal buffer, holding the enqueued elements.
         * bufend,     // Just a shortcut pointer to the end of the buffer.
                       // It helps me not constantly type (p->buffer + p->elem_size*p->capacity).
         * begin,      // Always points to the left-most/first-pushed element in the pipe.
         * end;        // Always points to the right-most/last-pushed element in the pipe.

    size_t producer_refcount;      // The number of producers currently in circulation.
    size_t consumer_refcount;      // The number of consumers currently in circulation.

    pthread_mutex_t m;             // The mutex guarding the WHOLE pipe. We use very
                                   // coarse-grained locking.

    pthread_cond_t  just_pushed;   // Signaled immediately after any push operation.
    pthread_cond_t  just_popped;   // Signaled immediately after any pop operation.
};

// Poor man's typedef. For more information, see DEF_NEW_FUNC's typedef.
struct producer { pipe_t pipe; };
struct consumer { pipe_t pipe; };

// Converts a pointer to either a producer or consumer into a suitable pipe_t*.
#define PIPIFY(producer_or_consumer) (&(producer_or_consumer)->pipe)

// The initial minimum capacity of the pipe. This can be overridden dynamically
// with pipe_reserve.
#ifdef DEBUG
#define DEFAULT_MINCAP  2
#else
#define DEFAULT_MINCAP  32
#endif

// Moves bufend to the end of the buffer, assuming buffer, capacity, and
// elem_size are all sane.
static inline void fix_bufend(pipe_t* p)
{
    p->bufend = p->buffer + p->elem_size * p->capacity;
}

// Does the buffer wrap around?
//   true  -> wrap
//   false -> nowrap
static inline bool wraps_around(const pipe_t* p)
{
    return p->begin > p->end;
}

// Is the pointer `p' within [left, right]?
static inline bool in_bounds(const void* left, const void* p, const void* right)
{
    return p >= left && p <= right;
}

// Wraps the begin (and possibly end) pointers of p to the beginning of the
// buffer if they've hit the end.
static inline char* wrap_if_at_end(char* p, char* begin, const char* end)
{
    return p == end ? begin : p;
}

static size_t next_pow2(size_t n)
{
    // I don't see why we would even try. Maybe a stacktrace will help.
    assert(n != 0);

    size_t top = ~(size_t)0; // 1111111..
    top = (top >> 1) + 1;    // 1000000..

    // If when we round up we will overflow our size_t, avoid rounding up and
    // exit early.
    if(n >= top)
        return n;

    // Therefore, at this point we have something that can be rounded up.
    // http://bits.stephan-brumme.com/roundUpToNextPowerOfTwo.html

    n--;

    for(size_t shift = 1; shift < sizeof(size_t)*CHAR_BIT; shift <<= 1)
        n |= n >> shift;

    n++;

    return n;
}

// You know all those assumptions we make about our data structure whenever we
// use it? This function checks them, and is called liberally through the
// codebase. It would be best to read this function over, as it also acts as
// documentation. Code AND documentation? What is this witchcraft?
static void check_invariants(const pipe_t* p)
{
    // Give me valid pointers or give me death!
    assert(p);

    // p->buffer may be NULL. When it is, we must have no issued consumers.
    // It's just a way to save memory when we've deallocated all consumers
    // and people are still trying to push like idiots.
    if(p->buffer == NULL)
    {
        assert(p->consumer_refcount == 0);
        return;
    }

    assert(p->bufend);
    assert(p->begin);
    assert(p->end);

    assert(p->elem_size != 0);

    assert(p->elem_count <= p->capacity && "There are more elements in the buffer than its capacity.");
    assert(p->bufend == p->buffer + p->elem_size*p->capacity && "This is axiomatic. Was fix_bufend not called somewhere?");

    assert(in_bounds(p->buffer, p->begin, p->bufend));
    assert(in_bounds(p->buffer, p->end, p->bufend));

    assert(p->min_cap >= DEFAULT_MINCAP);
    assert(p->min_cap <= p->max_cap);
    assert(p->capacity >= p->min_cap && p->capacity <= p->max_cap);

    assert(p->begin != p->bufend && "The begin pointer should NEVER point to the end of the buffer."
                    "If it does, it should have been automatically moved to the front.");

    // Ensure the size accurately reflects the begin/end pointers' positions.
    // Kindly refer to the diagram in struct pipe's documentation =)

    if(wraps_around(p)) //                   v     left half    v   v     right half     v
        assert(p->elem_size*p->elem_count == (p->end - p->buffer) + (p->bufend - p->begin));
    else
        assert(p->elem_size*p->elem_count == p->end - p->begin);
}

// Enforce is just assert, but runs the expression in release build, instead of
// filtering it out like assert would.
#ifdef NDEBUG
#define ENFORCE(expr) (void)(expr)
#else
#define ENFORCE assert
#endif

static inline void lock_pipe(pipe_t* p)
{
    ENFORCE(pthread_mutex_lock(&p->m) == 0);
    check_invariants(p);
}

static inline void unlock_pipe(pipe_t* p)
{
    check_invariants(p);
    ENFORCE(pthread_mutex_unlock(&p->m) == 0);
}

#define WHILE_LOCKED(stuff) do { lock_pipe(p); stuff; unlock_pipe(p); } while(0)

static inline void init_mutex(pthread_mutex_t* m)
{
    pthread_mutexattr_t attr;

    ENFORCE(pthread_mutexattr_init(&attr) == 0);
    ENFORCE(pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE) == 0);

    ENFORCE(pthread_mutex_init(m, &attr) == 0);
}

pipe_t* pipe_new(size_t elem_size, size_t limit)
{
    assert(elem_size != 0);

    assert(sizeof(pipe_t) == sizeof(consumer_t));
    assert(sizeof(consumer_t) == sizeof(producer_t));

    pipe_t* p = malloc(sizeof(pipe_t));

    if(p == NULL)
        return p;

    p->elem_size = elem_size;
    p->elem_count = 0;
    p->capacity =
    p->min_cap  = DEFAULT_MINCAP;
    p->max_cap  = limit ? next_pow2(max(limit, p->min_cap)) : ~(size_t)0;

    p->buffer =
    p->begin  =
    p->end    = malloc(p->elem_size * p->capacity);

    fix_bufend(p);

    p->producer_refcount =
    p->consumer_refcount = 1;    // Since we're issuing a pipe_t, it counts as both
                                 // a pusher and a popper since it can issue
                                 // new instances of both. Therefore, the count of
                                 // both starts at 1 - not the intuitive 0.

    init_mutex(&p->m);

    ENFORCE(pthread_cond_init(&p->just_pushed, NULL) == 0);
    ENFORCE(pthread_cond_init(&p->just_popped, NULL) == 0);

    check_invariants(p);

    return p;
}

// Yes, this is a total hack. What of it?
//
// What we do after incrementing the refcount is casting our pipe to the
// appropriate handle. Since the handle is defined with pipe_t as the
// first member (therefore lying at offset 0), we can secretly pass around
// our pipe_t structure without the rest of the world knowing it. This also
// keeps us from needlessly mallocing (and subsequently freeing) handles.
#define DEF_NEW_FUNC(type)                     \
    type##_t* pipe_##type##_new(pipe_t* p)     \
    {                                          \
        WHILE_LOCKED( ++p->type##_refcount; ); \
        return (type##_t*)p;                   \
    }

DEF_NEW_FUNC(producer)
DEF_NEW_FUNC(consumer)

#undef DEF_NEW_FUNC

static inline bool requires_deallocation(const pipe_t* p)
{
    return p->producer_refcount == 0 && p->consumer_refcount == 0;
}

static inline void deallocate(pipe_t* p)
{
    pthread_mutex_unlock(&p->m);

    pthread_mutex_destroy(&p->m);

    pthread_cond_destroy(&p->just_pushed);
    pthread_cond_destroy(&p->just_popped);

    free(p->buffer);
    free(p);
}

void pipe_free(pipe_t* p)
{
    WHILE_LOCKED(
        assert(p->producer_refcount > 0);
        assert(p->consumer_refcount > 0);

        --p->producer_refcount;
        --p->consumer_refcount;

        if(requires_deallocation(p))
            return deallocate(p);
    );
}

void pipe_producer_free(producer_t* handle)
{
    pipe_t* p = PIPIFY(handle);

    WHILE_LOCKED(
        assert(p->producer_refcount > 0);

        --p->producer_refcount;

        if(requires_deallocation(p))
            return deallocate(p);
    );
}

static inline void free_and_null(char** p)
{
    free(*p);
    *p = NULL;
}

void pipe_consumer_free(consumer_t* handle)
{
    pipe_t* p = PIPIFY(handle);

    WHILE_LOCKED(
        assert(p->consumer_refcount > 0);

        --p->consumer_refcount;

        // If this was the last consumer out of the gate, we can deallocate the
        // buffer. It has no use anymore.
        if(p->consumer_refcount == 0)
            free_and_null(&p->buffer);

        if(requires_deallocation(p))
            return deallocate(p);
    );
}

// Returns the end of the buffer (buf + number_of_bytes_copied).
static inline char* copy_pipe_into_new_buf(const pipe_t* p, char* buf, size_t bufsize)
{
    assert(bufsize >= p->elem_size * p->elem_count && "Trying to copy into a buffer that's too small.");
    check_invariants(p);

    if(wraps_around(p))
    {
        buf = offset_memcpy(buf, p->begin, p->bufend - p->begin);
        buf = offset_memcpy(buf, p->buffer, p->end - p->buffer);
    }
    else
    {
        buf = offset_memcpy(buf, p->begin, p->end - p->begin);
    }

    return buf;
}

static void resize_buffer(pipe_t* p, size_t new_size)
{
    check_invariants(p);

    // Let's NOT resize beyond our maximum capcity. Thanks =)
    if(new_size >= p->max_cap)
        new_size = p->max_cap;

    // I refuse to resize to a size smaller than what would keep all our
    // elements in the buffer or one that is smaller than the minimum capacity.
    if(new_size <= p->elem_count || new_size < p->min_cap)
        return;

    size_t new_size_in_bytes = new_size*p->elem_size;

    char* new_buf = malloc(new_size_in_bytes);
    p->end = copy_pipe_into_new_buf(p, new_buf, new_size_in_bytes);

    free(p->buffer);

    p->buffer = p->begin = new_buf;
    p->capacity = new_size;

    fix_bufend(p);

    check_invariants(p);
}

static inline void push_without_locking(pipe_t* p, const void* elems, size_t count)
{
    check_invariants(p);

    const size_t elem_count = p->elem_count,
                 elem_size  = p->elem_size;

    if(elem_count + count > p->capacity)
        resize_buffer(p, next_pow2(elem_count + count));

    // Since we've just grown the buffer (if necessary), we now KNOW we have
    // enough room for the push. So do it!
 
    size_t bytes_to_copy = count*elem_size;

    char* const buffer = p->buffer;
    char* const bufend = p->bufend;
    char*       end    = p->end;

    // If we currently have a nowrap buffer, we may have to wrap the new
    // elements. Copy as many as we can at the end, then start copying into the
    // beginning. This basically reduces the problem to only deal with wrapped
    // buffers, which can be dealt with using a single offset_memcpy.
    if(!wraps_around(p))
    {
        size_t at_end = min(bytes_to_copy, bufend - end);

        end = wrap_if_at_end(
                     offset_memcpy(end, elems, at_end),
                     p->buffer, bufend);

        elems = (const char*)elems + at_end;
        bytes_to_copy -= at_end;
    }

    // Now copy any remaining data...
    end = wrap_if_at_end(
              offset_memcpy(end, elems, bytes_to_copy),
              buffer, bufend
          );

    // ...and update the end pointer and count!
    p->end         = end;
    p->elem_count += count;

    check_invariants(p);
}

void pipe_push(producer_t* prod, const void* elems, size_t count)
{
    pipe_t* p = PIPIFY(prod);

    assert(elems && "Trying to push a NULL pointer into the pipe. That just won't do.");
    assert(p);

    if(count == 0)
        return;

    const size_t elem_size = p->elem_size,
                 max_cap   = p->max_cap;

    size_t elems_pushed = 0;

    WHILE_LOCKED(
        size_t elem_count        = p->elem_count;
        size_t consumer_refcount = p->consumer_refcount;

        // Wait for there to be enough room in the buffer for some new elements.
        for(; elem_count == max_cap && consumer_refcount > 0;
              elem_count        = p->elem_count,
              consumer_refcount = p->consumer_refcount)
            pthread_cond_wait(&p->just_popped, &p->m);

        // Don't perform an actual push if we have no consumers issued. The
        // buffer's been freed.
        if(consumer_refcount == 0)
            return unlock_pipe(p);

        // Push as many elements into the queue as possible.
        push_without_locking(p, elems,
            elems_pushed = min(count, max_cap - elem_count)
        );

        assert(elems_pushed > 0);
    );

    pthread_cond_broadcast(&p->just_pushed);

    // now get the rest of the elements, which we didn't have enough room for
    // in the pipe.
    return pipe_push(prod,
                     (const char*)elems + elems_pushed * elem_size,
                     count - elems_pushed);
}

// wow, I didn't even intend for the name to work like that...
static inline size_t pop_without_locking(pipe_t* p, void* target, size_t count)
{
    check_invariants(p);

          size_t elem_count = p->elem_count;
    const size_t elem_size  = p->elem_size;

    const size_t elems_to_copy   = min(count, elem_count);
          size_t bytes_remaining = elems_to_copy * elem_size;

    assert(bytes_remaining <= elem_count*elem_size);

    p->elem_count =
       elem_count = elem_count - elems_to_copy;

    char* const buffer = p->buffer;
    char* const bufend = p->bufend;
    char*       begin  = p->begin;

//  Copy [begin, min(bufend, begin + bytes_to_copy)) into target.
    {
        // Copy either as many bytes as requested, or the available bytes in
        // the RHS of a wrapped buffer - whichever is smaller.
        size_t first_bytes_to_copy = min(bytes_remaining, bufend - begin);

        target = offset_memcpy(target, begin, first_bytes_to_copy);

        bytes_remaining -= first_bytes_to_copy;
        begin            = wrap_if_at_end(
                               begin + first_bytes_to_copy,
                               buffer, bufend);
    }

    // If we're dealing with a wrap buffer, copy the remaining bytes
    // [buffer, buffer + bytes_to_copy) into target.
    if(bytes_remaining > 0)
    {
        memcpy(target, buffer, bytes_remaining);
        begin = wrap_if_at_end(begin + bytes_remaining, buffer, bufend);
    }

    // Since we cached begin on the stack, we need to reflect our changes back
    // on the pipe.
    p->begin = begin;

    check_invariants(p);

    // To conserve space like the good computizens we are, we'll shrink
    // our buffer if our memory usage efficiency drops below 25%. However,
    // since shrinking/growing the buffer is the most expensive part of a push
    // or pop, we only shrink it to bring us up to a 50% efficiency. A common
    // pipe usage pattern is sudden bursts of pushes and pops. This ensures it
    // doesn't get too time-inefficient.
    size_t capacity = p->capacity;

    if(elem_count <=    (capacity / 4))
        resize_buffer(p, capacity / 2);

    return elems_to_copy;
}

size_t pipe_pop(consumer_t* c, void* target, size_t count)
{
    pipe_t* p = PIPIFY(c);

    assert(target && "Why are we trying to pop elements out of a pipe and into a NULL buffer?");
    assert(p);

    size_t max_cap = p->max_cap;

    if(count > max_cap)
        count = max_cap;

    size_t ret;

    WHILE_LOCKED(
        size_t elem_count;

        // While we need more elements and there exists at least one producer...
        while((elem_count = p->elem_count) < count && p->producer_refcount > 0)
            pthread_cond_wait(&p->just_pushed, &p->m);

        ret = elem_count > 0
              ? pop_without_locking(p, target, count)
              : 0;
    );

    pthread_cond_broadcast(&p->just_popped);

    return ret;
}

void pipe_reserve(pipe_t* p, size_t count)
{
    if(p == NULL)
        return;

    if(count == 0)
        count = DEFAULT_MINCAP;

    size_t max_cap = p->max_cap;

    WHILE_LOCKED(
        if(count <= p->elem_count)
            return unlock_pipe(p);

        p->min_cap = min(count, max_cap);
        resize_buffer(p, count);
    );
}

// How many elements will we process at once?
#define BUFFER_SIZE     32

typedef struct {
    consumer_t* in;
    pipe_processor_t proc;
    const void* aux;
    producer_t* out;
} connect_data_t;

static void* process_pipe(void* param)
{
    connect_data_t p = *(connect_data_t*)param;
    free(param);

    char buf[BUFFER_SIZE * PIPIFY(p.in)->elem_size];
    size_t bytes_read;

    while((bytes_read = pipe_pop(p.in, buf, BUFFER_SIZE)))
        p.proc(buf, bytes_read, p.out, p.aux);

    pipe_consumer_free(p.in);
    pipe_producer_free(p.out);

    pthread_exit(0);
}

static void pipe_connect(consumer_t* in, pipe_processor_t proc, const void* aux, producer_t* out)
{
    connect_data_t* d = malloc(sizeof(connect_data_t));
    d->in = in;
    d->proc = proc;
    d->aux = aux;
    d->out = out;

    pthread_t t;

    pthread_create(&t, NULL, &process_pipe, d);
}

pipeline_t pipe_pipeline(const void* aux, ...)
{
    va_list va;
    va_start(va, aux);

    pipeline_t ret;
    consumer_t* last_pipe = NULL;

    // Create our first pipe.
    {
        pipe_t* p = pipe_new(va_arg(va, size_t), 0);
        ret.p = pipe_producer_new(p);
        last_pipe = pipe_consumer_new(p);
        pipe_free(p);
    }

    // Now create the rest.
    for(pipe_processor_t proc = va_arg(va, pipe_processor_t);
        proc != NULL;
        proc = va_arg(va, pipe_processor_t))
    {
        size_t pipe_size = va_arg(va, size_t);

        if(pipe_size == 0)
        {
            pipe_consumer_free(last_pipe);
            last_pipe = NULL;
            break;
        }

        pipe_t* pipe = pipe_new(pipe_size, 0);
        producer_t* in = pipe_producer_new(pipe);

        pipe_connect(last_pipe, proc, aux, in);
        pipe_producer_free(in);

        pipe_consumer_free(last_pipe);
        last_pipe = pipe_consumer_new(pipe);

        pipe_free(pipe);
    }

    va_end(va);

    ret.c = last_pipe;

    return ret;
}
