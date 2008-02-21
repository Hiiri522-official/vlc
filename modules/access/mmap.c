/*****************************************************************************
 * mmap.c: memory-mapped file input
 *****************************************************************************
 * Copyright © 2007-2008 Rémi Denis-Courmont
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <vlc/vlc.h>
#include <vlc_access.h>
#include <vlc_input.h>
#include <vlc_charset.h>
#include <vlc_interface.h>

#include <assert.h>

#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define FILE_MMAP_TEXT N_("Use file memory mapping")
#define FILE_MMAP_LONGTEXT N_( \
    "Try to use memory mapping to read files and block devices." )

static int Open (vlc_object_t *);
static void Close (vlc_object_t *);

vlc_module_begin();
    set_shortname (_("MMap"));
    set_description (_("Memory-mapped file input"));
    set_category (CAT_INPUT);
    set_subcategory (SUBCAT_INPUT_ACCESS);
    set_capability ("access2", 52);
    add_shortcut ("file");
    set_callbacks (Open, Close);

    add_bool ("file-mmap", VLC_TRUE, NULL,
              FILE_MMAP_TEXT, FILE_MMAP_LONGTEXT, VLC_TRUE);
vlc_module_end();

static block_t *Block (access_t *);
static int Seek (access_t *, int64_t);
static int Control (access_t *, int, va_list);

struct access_sys_t
{
    size_t page_size;
    size_t mtu;
    int    fd;
};

#define MMAP_SIZE (1 << 20)

static int Open (vlc_object_t *p_this)
{
    access_t *p_access = (access_t *)p_this;
    access_sys_t *p_sys;
    const char *path = p_access->psz_path;
    int fd;

    if (!var_CreateGetBool (p_this, "file-mmap"))
        return VLC_EGENERIC; /* disabled */

    STANDARD_BLOCK_ACCESS_INIT;

    if (!strcmp (p_access->psz_path, "-"))
        fd = dup (0);
    else
    {
        msg_Dbg (p_access, "opening file %s", path);
        fd = utf8_open (path, O_RDONLY | O_NOCTTY, 0666);
    }

    if (fd == -1)
    {
        msg_Warn (p_access, "cannot open %s: %m", path);
        goto error;
    }

    /* mmap() is only safe for regular and block special files.
     * For other types, it may be some idiosyncrasic interface (e.g. packet
     * sockets are a good example), if it works at all. */
    struct stat st;

    if (fstat (fd, &st))
    {
        msg_Err (p_access, "cannot stat %s: %m", path);
        goto error;
    }

    if (!S_ISREG (st.st_mode) && !S_ISBLK (st.st_mode))
    {
        msg_Dbg (p_access, "skipping non regular file %s", path);
        goto error;
    }

    /* Autodetect mmap() support */
    if (st.st_size > 0)
    {
        void *addr = mmap (NULL, 1, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (addr != MAP_FAILED)
            munmap (addr, 1);
        else
            goto error;
    }

    p_sys->page_size = sysconf (_SC_PAGE_SIZE);
    p_sys->mtu = MMAP_SIZE;
    if (p_sys->mtu < p_sys->page_size)
        p_sys->mtu = p_sys->page_size;
    p_sys->fd = fd;

    p_access->info.i_size = st.st_size;
    posix_fadvise (fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    return VLC_SUCCESS;

error:
    if (fd != -1)
        close (fd);
    free (p_sys);
    return VLC_EGENERIC;
}


static void Close (vlc_object_t * p_this)
{
    access_t *p_access = (access_t *)p_this;
    access_sys_t *p_sys = p_access->p_sys;

    close (p_sys->fd); /* don't care about error when only reading */
    free (p_sys);
}

static block_t *Block (access_t *p_access)
{
    access_sys_t *p_sys = p_access->p_sys;
#ifndef NDEBUG
    int64_t dbgpos = lseek (p_sys->fd, 0, SEEK_CUR);
    if (dbgpos != p_access->info.i_pos)
        msg_Err (p_access, "position: 0x%08llx instead of 0x%08llx",
                 p_access->info.i_pos, dbgpos);
#endif

    if ((uint64_t)p_access->info.i_pos >= (uint64_t)p_access->info.i_size)
    {
        /* End of file - check if file size changed... */
        struct stat st;

        if ((fstat (p_sys->fd, &st) == 0)
         && (st.st_size != p_access->info.i_size))
        {
            p_access->info.i_size = st.st_size;
            p_access->info.i_update |= INPUT_UPDATE_SIZE;
        }

        /* Really at end of file then */
        if ((uint64_t)p_access->info.i_pos >= (uint64_t)p_access->info.i_size)
        {
            p_access->info.b_eof = VLC_TRUE;
            msg_Dbg (p_access, "at end of memory mapped file");
            return NULL;
        }
    }

    const uintptr_t page_mask = p_sys->page_size - 1;
    /* Start the mapping on a page boundary: */
    off_t  outer_offset = p_access->info.i_pos & ~page_mask;
    /* Skip useless bytes at the beginning of the first page: */
    size_t inner_offset = p_access->info.i_pos & page_mask;
    /* Map no more bytes than remain: */
    size_t length = p_sys->mtu;
    if (outer_offset + length > p_access->info.i_size)
        length = p_access->info.i_size - outer_offset;

    assert (outer_offset <= p_access->info.i_pos);          /* and */
    assert (p_access->info.i_pos < p_access->info.i_size); /* imply */
    assert (outer_offset < p_access->info.i_size);         /* imply */
    assert (length > 0);

    /* NOTE: We use PROT_WRITE and MAP_PRIVATE so that the block can be
     * modified down the chain, without messing up with the underlying
     * original file. This does NOT need open write permission. */
    void *addr = mmap (NULL, length, PROT_READ|PROT_WRITE, MAP_PRIVATE,
                       p_sys->fd, outer_offset);
    if (addr == MAP_FAILED)
    {
        msg_Err (p_access, "memory mapping failed (%m)");
        intf_UserFatal (p_access, VLC_FALSE, _("File reading failed"),
                        _("VLC could not read the file."));
        msleep (INPUT_ERROR_SLEEP);
        return NULL;
    }

    block_t *block = block_mmap_Alloc (addr, length);
    if (block == NULL)
        return NULL;

    block->p_buffer += inner_offset;
    block->i_buffer -= inner_offset;

#ifndef NDEBUG
    msg_Dbg (p_access, "mapped 0x%lx bytes at %p from offset 0x%lx",
             (unsigned long)length, addr, (unsigned long)outer_offset);

    /* Compare normal I/O with memory mapping */
    char *buf = malloc (block->i_buffer);
    ssize_t i_read = read (p_sys->fd, buf, block->i_buffer);

    if (i_read != (ssize_t)block->i_buffer)
        msg_Err (p_access, "read %u instead of %u bytes", (unsigned)i_read,
                 (unsigned)block->i_buffer);
    if (memcmp (buf, block->p_buffer, block->i_buffer))
        msg_Err (p_access, "inconsistent data buffer");
    free (buf);
#endif

    p_access->info.i_pos = outer_offset + length;
    return block;
}


static int Seek (access_t *p_access, int64_t i_pos)
{
    p_access->info.i_pos = i_pos;
    p_access->info.b_eof = VLC_FALSE;
    return VLC_SUCCESS;
}


static int Control (access_t *p_access, int query, va_list args)
{
    access_sys_t *p_sys = p_access->p_sys;

    switch (query)
    {
        case ACCESS_CAN_SEEK:
        case ACCESS_CAN_FASTSEEK:
        case ACCESS_CAN_PAUSE:
        case ACCESS_CAN_CONTROL_PACE:
            *((vlc_bool_t *)va_arg (args, vlc_bool_t *)) = VLC_TRUE;
            return VLC_SUCCESS;

        case ACCESS_GET_MTU:
            *((int *)va_arg (args, int *)) = p_sys->mtu;
            return VLC_SUCCESS;

        case ACCESS_GET_PTS_DELAY:
            *((int64_t *)va_arg (args, int64_t *)) = DEFAULT_PTS_DELAY;
            return VLC_SUCCESS;

        case ACCESS_GET_TITLE_INFO:
        case ACCESS_GET_META:
            break;

        case ACCESS_SET_PAUSE_STATE:
            return VLC_SUCCESS;

        case ACCESS_SET_TITLE:
        case ACCESS_SET_SEEKPOINT:
        case ACCESS_SET_PRIVATE_ID_STATE:
        case ACCESS_SET_PRIVATE_ID_CA:
        case ACCESS_GET_PRIVATE_ID_STATE:
        case ACCESS_GET_CONTENT_TYPE:
            break;

        default:
            msg_Warn (p_access, "unimplemented query %d in control", query);
    }

    return VLC_EGENERIC;
}
