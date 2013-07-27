/*****************************************************************************
 * ringbuf.c: Stream ring buffer in memory
 *****************************************************************************
 *
 * Authors: Rui Zhang <bbcallen _AT_ gmail _DOT_ com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>

#include <assert.h>
#include <errno.h>

#include <vlc_threads.h>
#include <vlc_arrays.h>
#include <vlc_stream.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define ENABLE_TEXT N_("")
#define ENABLE_LONGTEXT N_("")

vlc_module_begin ()
    set_description (N_("Ring stream buffer"))
    set_category (CAT_INPUT)
    set_subcategory (SUBCAT_INPUT_STREAM_FILTER)
    set_capability ("stream_filter", 1)
    add_shortcut( "ringbuf", "asyncbuf" )
    add_bool( "ringbuf-enable", false, ENABLE_TEXT, ENABLE_LONGTEXT, false )
    set_callbacks (Open, Close)
vlc_module_end ()

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

#define RING_BLOCK_SIZE ( 1 * 1024 * 1024 )
#define RING_BLOCK_COUNT ( 10 )
#define RING_TOTAL_CAPACITY ( RING_BLOCK_SIZE * RING_BLOCK_COUNT )

// assert(RING_BUFF_RW_GUARD_GAP + RING_BUFF_SEEK_GUARD_GAP < RING_BLOCK_SIZE)
#define RING_BUFF_RW_GUARD_GAP ( 1024 )
#define RING_BUFF_SEEK_GUARD_GAP ( 1 * 1024 * 1024 )

#define RING_SEEK_THRESHOLD ( 1 * 1024 * 1024 )

#define BYTES_PER_READ ( 32 * 1024 )
#define SHORT_SEEK_RANGE ( 128 * 1024 )

#define COND_POLL_TIME ( 1000 * 1000 )

struct stream_sys_t
{
    uint64_t        i_stream_size;
    bool            b_can_fastseek;
    bool            b_can_seek;
    volatile bool   b_error;
    volatile bool   b_abort;

    vlc_mutex_t     buffer_mutex;
    vlc_cond_t      wakeup_read_cond;
    vlc_cond_t      wakeup_write_cond;

    vlc_thread_t    buffer_thread;

    uint8_t        *p_buffer[RING_BLOCK_COUNT];
    int             i_capacity;

    /* cached index/size, data not been overwriten */
    volatile int        i_cache_index;
    volatile int        i_cache_size;
    volatile uint64_t   i_cache_offset;

    /* buffered index/size, data not been read */
    volatile int        i_buffer_size;

    volatile int        i_read_index;
    volatile int        i_write_index;

    volatile uint64_t   i_seek_pos;
    volatile bool       b_seek_request;
    volatile uint64_t   i_stream_offset;
    volatile bool       b_buffered_eos;

    /* Temporary buffer for Peek() */
    uint8_t             *p_temp_peek;
    unsigned int         i_temp_peek_capacity;
};

static int Read   ( stream_t *p_stream, void *p_buffer, unsigned int i_read );
static int Peek   ( stream_t *p_stream, const uint8_t **pp_peek, unsigned int i_peek );
static int Control( stream_t *p_stream, int i_query, va_list args );

static void* BufferThread(void *);

static stream_sys_t *sys_Alloc( void );
static void sys_Free( stream_sys_t** );

#define msg_RBuf msg_Err

/****************************************************************************
 * Open/Close
 ****************************************************************************/

static int Open( vlc_object_t *obj )
{
    stream_t *p_stream = (stream_t *)obj;
    stream_sys_t *p_sys = NULL;

    assert( RING_BUFF_RW_GUARD_GAP + RING_BUFF_SEEK_GUARD_GAP < RING_TOTAL_CAPACITY );
    assert( p_stream );
    assert( p_stream->p_source );

    if( !p_stream->p_source )
        goto EXIT_FAIL;

    /* FIXME: Need a better way to check if already loaded in stream filter chain */
    if( p_stream->p_source->p_source )
        goto EXIT_FAIL;

    bool b_enabled = var_InheritBool( p_stream, "ringbuf-enable" );
    if( !b_enabled )
    {
        msg_Info( p_stream, "ringbuf: disable ringbuf" );
        goto EXIT_FAIL;
    }

    p_sys = sys_Alloc();
    if( !p_sys )
        goto EXIT_FAIL;

    /* get stream information */
    p_sys->i_stream_size = stream_Size( p_stream->p_source );
    if( p_sys->i_stream_size <= 0 )
    {
        msg_Err( p_stream, "ringbuf: stream unknown size" );
        goto EXIT_FAIL;
    }
    msg_Info( p_stream, "ringbuf: stream size: %"PRId64, p_sys->i_stream_size );

    stream_Control( p_stream->p_source, STREAM_CAN_FASTSEEK, &p_sys->b_can_fastseek );
    stream_Control( p_stream->p_source, STREAM_CAN_SEEK, &p_sys->b_can_seek );

    /* */
    p_stream->p_sys = p_sys;
    p_stream->pf_read = Read;
    p_stream->pf_peek = Peek;
    p_stream->pf_control = Control;

    if(VLC_SUCCESS != vlc_clone( &p_sys->buffer_thread, BufferThread, p_stream, VLC_THREAD_PRIORITY_INPUT ) )
        goto EXIT_FAIL;

    msg_Info( p_stream, "ringbuf: loaded" );
    return VLC_SUCCESS;
EXIT_FAIL:
    msg_Warn( p_stream, "ringbuf: not loaded" );
    sys_Free( &p_sys );

    return VLC_EGENERIC;
}

static void Close( vlc_object_t *obj )
{
    stream_t *p_stream = (stream_t *)obj;
    stream_sys_t *p_sys = p_stream->p_sys;

    msg_Info( p_stream, "ringbuf: close" );

    if( p_sys )
    {
        /* wakeup buffer thread */
        p_sys->b_abort = true;

        vlc_mutex_lock( &p_sys->buffer_mutex );
        mutex_cleanup_push( &p_sys->buffer_mutex );
        vlc_cond_broadcast( &p_sys->wakeup_write_cond );
        vlc_cond_broadcast( &p_sys->wakeup_read_cond );
        vlc_cleanup_run( );

        vlc_cancel( p_sys->buffer_thread );
        vlc_join( p_sys->buffer_thread, NULL );
    }

    sys_Free( &p_stream->p_sys );
}

/****************************************************************************
 * Alloc/Free
 ****************************************************************************/
static stream_sys_t *sys_Alloc( void )
{
    stream_sys_t *p_sys = (stream_sys_t *) malloc( sizeof(stream_sys_t) );
    if( !p_sys )
        return NULL;

    memset( p_sys, 0, sizeof(stream_sys_t) );

    for( int i = 0; i < RING_BLOCK_COUNT; ++i )
    {
        p_sys->p_buffer[i] = (uint8_t *) malloc( RING_BLOCK_SIZE );
        if( p_sys->p_buffer[i] == NULL )
        {
            sys_Free( &p_sys );
            return NULL;
        }
    }

    vlc_mutex_init( &p_sys->buffer_mutex );
    vlc_cond_init( &p_sys->wakeup_read_cond );
    vlc_cond_init( &p_sys->wakeup_write_cond );

    return p_sys;
}

static void sys_Free( stream_sys_t** pp_sys )
{
    stream_sys_t *p_sys = *pp_sys;
    if( p_sys )
    {
        if( p_sys->p_temp_peek )
        {
            free( p_sys->p_temp_peek );
            p_sys->p_temp_peek = NULL;
            p_sys->i_temp_peek_capacity = 0;
        }

        for( int i = 0; i < RING_BLOCK_COUNT; ++i )
        {
            free( p_sys->p_buffer[i] );
            p_sys->p_buffer[i] = NULL;
        }

        vlc_cond_destroy( &p_sys->wakeup_read_cond );
        vlc_cond_destroy( &p_sys->wakeup_write_cond );
        vlc_mutex_destroy( &p_sys->buffer_mutex );

        free( p_sys );
        *pp_sys = NULL;
    }
}

/* must be locked with p_sys->buffer_mutex */
static int WaitBufferForRead_l( stream_t *p_stream, int i_read )
{
    stream_sys_t *p_sys = p_stream->p_sys;
    bool b_start_wait = false;
    mtime_t start_wait_time = 0;
    if( i_read <= 0 )
        return i_read;

    // wait until enough data to read
    int i_data_ready = i_read;
    while( p_sys->b_seek_request || i_read > p_sys->i_buffer_size )
    {
        if( p_sys->b_abort )
        {
            msg_Warn( p_stream, "ringbuf: WaitBufferForRead_l(%d) abort", i_read );
            i_data_ready = -1;
            break;
        }

        if( p_sys->b_error )
        {
            msg_Warn( p_stream, "ringbuf: WaitBufferForRead_l(%d) error", i_read );
            i_data_ready = -1;
            break;
        }

        if( p_sys->b_buffered_eos )
        {
            msg_RBuf( p_stream, "ringbuf: WaitBufferForRead_l(%d) eos: %"PRId64", %"PRId64,
                      i_read, p_sys->i_stream_offset,
                      p_sys->i_stream_offset + p_sys->i_buffer_size );
            i_data_ready = p_sys->i_buffer_size;
            break;
        }

        if( !b_start_wait )
        {
            msg_RBuf( p_stream, "ringbuf: WaitBufferForRead_l(%d) wait start: %"PRId64" + %d (seek: %d)",
                      i_read, p_sys->i_stream_offset,
                      p_sys->i_buffer_size, p_sys->b_seek_request ? 1 : 0 );
            b_start_wait = true;
            start_wait_time = mdate();
        }

        vlc_testcancel();
        if( !p_sys->b_seek_request )
        {
            vlc_cond_broadcast( &p_sys->wakeup_write_cond );
        }

        vlc_cond_timedwait( &p_sys->wakeup_read_cond, &p_sys->buffer_mutex, mdate() + COND_POLL_TIME );
    }

    if( b_start_wait )
    {
        mtime_t wait_time = mdate() - start_wait_time;
        msg_RBuf( p_stream, "ringbuf: WaitBufferForRead_l(%d) wait end (%"PRId64" ms): %"PRId64" + %d",
                  i_read, (int64_t) wait_time / 1000, p_sys->i_stream_offset,
                  p_sys->i_buffer_size );
    }

    return i_data_ready;
}

/* must be locked with p_sys->buffer_mutex */
static int PeekFromBuffer_l( stream_t *p_stream, void *p_buffer, int i_read )
{
    stream_sys_t *p_sys = p_stream->p_sys;

    int i_data_ready = WaitBufferForRead_l( p_stream, i_read );
    if( i_data_ready < 0 )
    {
        msg_Warn( p_stream, "ringbuf: PeekFromBuffer_l() interrupted or eos" );
        return i_data_ready;
    }
    if( p_sys->i_buffer_size <= 0 )
    {
        msg_Err( p_stream, "ringbuf: PeekFromBuffer_l() unexpected i_buffer_size <= 0" );
        return i_data_ready;
    }
    // assert( p_sys->i_buffer_size > 0 );

    int i_to_read = i_data_ready;
    if( i_to_read > p_sys->i_buffer_size )
        i_to_read = p_sys->i_buffer_size;

    /* beyond last byte */
    int i_read_index  = p_sys->i_read_index;
    int i_data_filled = 0;
    while( i_data_filled < i_to_read )
    {
        int i_block_id          = i_read_index / RING_BLOCK_SIZE;
        int i_offset_in_block   = i_read_index % RING_BLOCK_SIZE;
        int i_max_read_in_block = RING_BLOCK_SIZE - i_offset_in_block;

        int i_copy = i_to_read - i_data_filled;
        if( i_copy > i_max_read_in_block )
            i_copy = i_max_read_in_block;

        if( p_buffer )
            memcpy( (uint8_t*)p_buffer + i_data_filled, p_sys->p_buffer[i_block_id] + i_offset_in_block, i_copy );

        i_data_filled += i_copy;
        i_read_index += i_copy;
        i_read_index %= RING_TOTAL_CAPACITY;
    }

    return i_data_filled;
}

/* must be locked with p_sys->buffer_mutex */
static int ReadFromBuffer_l( stream_t *p_stream, void *p_buffer, int i_read )
{
    stream_sys_t *p_sys = p_stream->p_sys;

    int i_data_peeked = PeekFromBuffer_l( p_stream, p_buffer, i_read );
    if( i_data_peeked < 0 )
    {
        msg_Warn( p_stream, "ringbuf: ReadFromBuffer_l() interrupted or eos" );
        return i_data_peeked;
    }

    p_sys->i_buffer_size -= i_data_peeked;
    p_sys->i_read_index += i_data_peeked;
    p_sys->i_read_index %= RING_TOTAL_CAPACITY;
    p_sys->i_stream_offset += i_data_peeked;
    vlc_cond_broadcast( &p_sys->wakeup_write_cond );

    return i_data_peeked;
}

/* must be locked with p_sys->buffer_mutex */
static int WaitBufferForWrite_l( stream_t *p_stream, int i_write )
{
    stream_sys_t *p_sys = p_stream->p_sys;
    bool b_start_wait = false;
    mtime_t start_wait_time = 0;
    if( i_write <= 0 )
        return i_write;

    // wait until enough space to write
    int i_buffer = i_write;
    while( p_sys->i_buffer_size + i_buffer > RING_TOTAL_CAPACITY - RING_BUFF_RW_GUARD_GAP - RING_BUFF_SEEK_GUARD_GAP)
    {
        if( p_sys->b_abort )
        {
            msg_Warn( p_stream, "ringbuf: WaitBufferForWrite_l(%d) abort", i_write );
            i_buffer = -1;
            break;
        }

        if( p_sys->b_error )
        {
            msg_Warn( p_stream, "ringbuf: WaitBufferForWrite_l(%d) error", i_write );
            i_buffer = -1;
            break;
        }

        if( p_sys->b_seek_request && p_sys->i_buffer_size + i_buffer < RING_TOTAL_CAPACITY - RING_BUFF_RW_GUARD_GAP )
        {
            msg_Warn( p_stream, "ringbuf: WaitBufferForWrite_l(%d) write to seek-gap", i_write );
            break;
        }

        if( !b_start_wait )
        {
            msg_RBuf( p_stream, "ringbuf: WaitBufferForWrite_l(%d) wait start: %d",
                      i_buffer, p_sys->i_buffer_size );
            b_start_wait = true;
            start_wait_time = mdate();
        }

        vlc_testcancel();
        vlc_cond_broadcast( &p_sys->wakeup_read_cond );
        vlc_cond_timedwait( &p_sys->wakeup_write_cond, &p_sys->buffer_mutex, mdate() + COND_POLL_TIME );
    }

    if( b_start_wait )
    {
        mtime_t wait_time = mdate() - start_wait_time;
        msg_RBuf( p_stream, "ringbuf: WaitBufferForWrite_l(%d) wait end(%"PRId64" ms): %d",
                  i_buffer, (int64_t) wait_time / 1000, p_sys->i_buffer_size );
    }

    return i_buffer;
}

/* must be locked with p_sys->buffer_mutex */
static int WriteToBuffer_l( stream_t *p_stream, void *p_buffer, int i_write )
{
    stream_sys_t *p_sys = p_stream->p_sys;
    assert( p_buffer );

    int i_buffer = WaitBufferForWrite_l( p_stream, i_write );
    if( i_buffer < 0 )
    {
        msg_Warn( p_stream, "ringbuf: WriteToBuffer_l() interrupted or eos" );
        return i_buffer;
    }
    assert( i_write <= ( RING_TOTAL_CAPACITY - p_sys->i_buffer_size ) );

    /* beyond last byte */
    int i_write_index = p_sys->i_write_index;
    int i_data_filled = 0;
    while( i_data_filled < i_write )
    {
        int i_block_id           = i_write_index / RING_BLOCK_SIZE;
        int i_offset_in_block    = i_write_index % RING_BLOCK_SIZE;
        int i_max_write_in_block = RING_BLOCK_SIZE - i_offset_in_block;

        int i_copy = i_write - i_data_filled;
        if( i_copy > i_max_write_in_block )
            i_copy = i_max_write_in_block;

        if( p_buffer )
            memcpy( p_sys->p_buffer[i_block_id] + i_offset_in_block,
                    (uint8_t *) p_buffer + i_data_filled, i_copy );

        i_data_filled += i_copy;
        i_write_index += i_copy;
        i_write_index %= RING_TOTAL_CAPACITY;
    }

    p_sys->i_buffer_size += i_data_filled;
    p_sys->i_write_index += i_data_filled;
    p_sys->i_write_index %= RING_TOTAL_CAPACITY;

    p_sys->i_cache_size += i_data_filled;
    if( p_sys->i_cache_size > RING_TOTAL_CAPACITY )
    {
        int i_diff = p_sys->i_cache_size - RING_TOTAL_CAPACITY - RING_BUFF_RW_GUARD_GAP - RING_BUFF_SEEK_GUARD_GAP;
        p_sys->i_cache_offset += i_diff;
        p_sys->i_cache_size -= i_diff;
        p_sys->i_cache_index = ( p_sys->i_cache_index + i_diff ) % RING_TOTAL_CAPACITY;;
    }

    if( !p_sys->b_seek_request )
        vlc_cond_broadcast( &p_sys->wakeup_read_cond );

    return i_data_filled;
}

/****************************************************************************
 * Buffer Thread
 *
 * try to be cancellation-safe
 ****************************************************************************/
static void* BufferThread(void *p_this)
{
    stream_t *p_stream = (stream_t *) p_this;
    stream_sys_t *p_sys = p_stream->p_sys;
    stream_t *p_source = p_stream->p_source;
    uint8_t buf[BYTES_PER_READ];

    while( !p_sys->b_abort && !p_sys->b_error )
    {
        uint64_t i_source_offset = stream_Tell( p_stream->p_source );
        if( i_source_offset >= p_sys->i_stream_size )
        {
            msg_Info( p_stream, "ringbuf: buffered to the EOS" );
            p_sys->b_buffered_eos = true;
        }

        if( p_sys->b_buffered_eos )
        {
            msg_Info( p_stream, "ringbuf: EOS, wait for seek or exit" );

            vlc_mutex_lock( &p_sys->buffer_mutex );
            mutex_cleanup_push( &p_sys->buffer_mutex );
            p_sys->b_buffered_eos = true;
            while( !p_sys->b_abort && !p_sys->b_error && !p_sys->b_seek_request )
            {
                vlc_cond_timedwait( &p_sys->wakeup_write_cond, &p_sys->buffer_mutex, mdate() + COND_POLL_TIME );
            }
            vlc_cleanup_run( );

            if( p_sys->b_abort || p_sys->b_error )
                break;

            p_sys->b_buffered_eos = false;
        }

        if( p_sys->b_seek_request )
        {
            bool b_long_seek = false;
            bool b_seek_ready = false;

            vlc_mutex_lock( &p_sys->buffer_mutex );
            mutex_cleanup_push( &p_sys->buffer_mutex );

            uint64_t cached_start_offset = p_sys->i_cache_offset;
            uint64_t cached_end_offset = cached_start_offset + p_sys->i_cache_size;

            if( p_sys->i_seek_pos < cached_start_offset || p_sys->i_seek_pos >= cached_end_offset + RING_SEEK_THRESHOLD )
            {
                msg_Info( p_stream, "long seek %"PRId64" beyond [%"PRId64", %"PRId64")",
                          p_sys->i_seek_pos,
                          cached_start_offset,
                          cached_end_offset );
                b_long_seek = true;
                b_seek_ready = true;
            }
            else if( p_sys->i_seek_pos < cached_end_offset )
            {
                msg_Info( p_stream, "short seek %"PRId64" within [%"PRId64", %"PRId64")",
                          p_sys->i_seek_pos,
                          cached_start_offset,
                          cached_end_offset );
                b_seek_ready = true;
            }
            else
            {
                // read through until short seek is ready
                msg_RBuf( p_stream, "middle seek %"PRId64" beyond [%"PRId64", %"PRId64")",
                          p_sys->i_seek_pos,
                          cached_start_offset,
                          cached_end_offset );

                // clear buffer to void deadlock in WriteToBuffer_l later
                p_sys->i_read_index = p_sys->i_write_index;
                p_sys->i_buffer_size = 0;
                b_seek_ready = false;
            }

            vlc_cleanup_run( );

            if( b_seek_ready )
            {
                if( b_long_seek )
                {
                    vlc_testcancel();
                    msg_RBuf( p_stream, "stream seek to %"PRId64, p_sys->i_seek_pos );
                    int i_seek_ret = stream_Seek( p_stream->p_source, p_sys->i_seek_pos );
                    if( i_seek_ret < 0 )
                    {
                        p_sys->b_error = true;
                        break;
                    }
                }

                vlc_mutex_lock( &p_sys->buffer_mutex );
                mutex_cleanup_push( &p_sys->buffer_mutex );

                i_source_offset = p_sys->i_seek_pos;
                p_sys->i_stream_offset = i_source_offset;

                if( b_long_seek )
                {
                    // long seek: reset ring buffer read/write index
                    p_sys->i_cache_index = 0;
                    p_sys->i_cache_size = 0;
                    p_sys->i_cache_offset = i_source_offset;

                    p_sys->i_read_index = i_source_offset % RING_TOTAL_CAPACITY;
                    p_sys->i_write_index = i_source_offset % RING_TOTAL_CAPACITY;
                    p_sys->i_buffer_size = 0;
                }
                else
                {
                    // short seek: only reset read index
                    p_sys->i_read_index = i_source_offset % RING_TOTAL_CAPACITY;
                    p_sys->i_buffer_size = ( p_sys->i_write_index + RING_TOTAL_CAPACITY - p_sys->i_read_index ) % RING_TOTAL_CAPACITY;
                }

                p_sys->b_seek_request = false;
                p_sys->i_seek_pos = 0;

                if( !b_long_seek )
                    vlc_cond_broadcast( &p_sys->wakeup_read_cond );

                vlc_cleanup_run( );
            }
        }

        vlc_testcancel();
        int i_step_read = sizeof( buf );
        int i_read_ret = stream_Read( p_source, buf, i_step_read );
        if( i_read_ret < 0 )
        {
            p_sys->b_error = true;
            break;
        }

        int i_fill_ret = 0;

        vlc_mutex_lock( &p_sys->buffer_mutex );
        mutex_cleanup_push( &p_sys->buffer_mutex );

        if( i_read_ret > 0 )
            i_fill_ret = WriteToBuffer_l( p_stream, buf, i_read_ret );

        if( i_read_ret < i_step_read )
        {
            msg_Info( p_stream, "ringbuf: unexpected EOS when WriteToBuffer_l(%d) = %d", i_step_read, i_read_ret );
            p_sys->b_buffered_eos = true;
        }

        vlc_cleanup_run( );

        if( i_fill_ret < 0 )
        {
            p_sys->b_error = true;
            break;
        }
    }
    p_sys->b_buffered_eos = true;

    vlc_mutex_lock( &p_sys->buffer_mutex );
    vlc_cond_broadcast( &p_sys->wakeup_read_cond );
    vlc_mutex_unlock( &p_sys->buffer_mutex );

    return NULL;
}

/****************************************************************************
 * Read/Peek/Control
 ****************************************************************************/
static int Read( stream_t *p_stream, void *p_buffer, unsigned int i_read )
{
    int i_result = 0;
    stream_sys_t *p_sys = p_stream->p_sys;
    assert( p_stream->p_source );

    vlc_mutex_lock( &p_sys->buffer_mutex );
    mutex_cleanup_push( &p_sys->buffer_mutex );

    i_result = ReadFromBuffer_l( p_stream, p_buffer, i_read );
    if( i_result < 0 )
    {
        msg_Warn( p_stream, "ringbuf: Read(%d) interrupted or eos (%d)", i_read, i_result );
    }
    else if( (unsigned int) i_result < i_read )
    {
        msg_Warn( p_stream, "ringbuf: Read(%d) eos (%d)", i_read, i_result );
    }

    vlc_cleanup_run();

    return i_result;
}

static int Peek( stream_t *p_stream, const uint8_t **pp_peek, unsigned int i_peek )
{
    int i_result = 0;
    stream_sys_t *p_sys = p_stream->p_sys;
    uint8_t *p_temp_peek = p_sys->p_temp_peek;
    assert( p_stream->p_source );

    if( p_sys->i_temp_peek_capacity < i_peek )
    {
        p_temp_peek = realloc( p_sys->p_temp_peek, i_peek );
        if( !p_temp_peek )
            return VLC_ENOMEM;
        p_sys->p_temp_peek = p_temp_peek;
    }

    vlc_mutex_lock( &p_sys->buffer_mutex );
    mutex_cleanup_push( &p_sys->buffer_mutex );

    i_result = PeekFromBuffer_l( p_stream, p_temp_peek, i_peek );
    if( i_result < 0 ) {
        msg_Warn( p_stream, "ringbuf: Peek() interrupted or eos (%d)", i_result );
    }
    else
    {
        *pp_peek = p_sys->p_temp_peek;
    }

    vlc_cleanup_run();

    return i_result;
}

static int Control( stream_t *p_stream, int i_query, va_list args )
{
    stream_sys_t *p_sys = p_stream->p_sys;

    assert( p_stream->p_source );
    switch (i_query)
    {
        case STREAM_CAN_FASTSEEK:
        {
            *(va_arg (args, bool *)) = false;
            break;
        }
        case STREAM_CAN_SEEK:
        {
            *(va_arg (args, bool *)) = p_sys->b_can_seek;
            break;
        }
        case STREAM_GET_POSITION:
        {
            vlc_mutex_lock( &p_sys->buffer_mutex );
            if( p_sys->b_seek_request )
                *(va_arg (args, uint64_t *)) = p_sys->i_seek_pos;
            else
                *(va_arg (args, uint64_t *)) = p_sys->i_stream_offset;
            vlc_mutex_unlock( &p_sys->buffer_mutex );
            break;
        }
        case STREAM_SET_POSITION:
        {
            if( p_sys->b_can_seek )
            {
                vlc_mutex_lock( &p_sys->buffer_mutex );
                p_sys->i_seek_pos = (uint64_t)va_arg(args, uint64_t);
                p_sys->b_seek_request = true;
                vlc_cond_broadcast( &p_sys->wakeup_write_cond );
                vlc_mutex_unlock( &p_sys->buffer_mutex );
                break;
            }
            else
            {
                return VLC_EGENERIC;
            }
        }
        case STREAM_GET_SIZE:
        {
            *(va_arg (args, uint64_t *)) = p_sys->i_stream_size;
            break;
        }
        case STREAM_GET_CACHED_SIZE:
        {
            vlc_mutex_lock( &p_sys->buffer_mutex );
            *(va_arg (args, uint64_t *)) = p_sys->i_stream_offset + p_sys->i_buffer_size;
            vlc_mutex_unlock( &p_sys->buffer_mutex );
            break;
        }
        default:
        {
            return VLC_EGENERIC;
        }
    }
    return VLC_SUCCESS;
}