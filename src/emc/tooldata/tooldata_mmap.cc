/*
** Copyright: 2020
** Author:    Dewey Garrett <dgarrett@panix.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include "rtapi_mutex.h"
#include "tooldata.hh"

#define TOOL_MMAP_FILENAME "/tmp/tool.mmap"
#define TOOL_MMAP_MODE     0600
#define TOOL_MMAP_CREATOR_OPEN_FLAGS  O_RDWR | O_CREAT | O_TRUNC
#define TOOL_MMAP_USER_OPEN_FLAGS     O_RDWR

static int           creator_fd;
static char*         tool_mmap_base = 0;
static EMC_TOOL_STAT const *toolstat;

typedef struct {
    rtapi_mutex_t   mutex;
    unsigned int    last_index;
    changer_t       changer;
} tooldata_header_t;

/* mmap region:
**   1) header
**   2) CANON_TOOL_TABLE items (howmany=CANON_POCKETS_MAX)
*/

//---------------------------------------------------------------------
#define TOOL_MMAP_HEADER_OFFSET 0
#define TOOL_MMAP_HEADER_SIZE sizeof(tooldata_header_t)

#define TOOL_MMAP_SIZE    TOOL_MMAP_HEADER_SIZE + \
                          CANON_POCKETS_MAX * sizeof(struct CANON_TOOL_TABLE)

#define TOOL_MMAP_STRIDE  sizeof(CANON_TOOL_TABLE)
//---------------------------------------------------------------------
#define TOOL_MMAP_HEADER   (tool_mmap_base \
                           + TOOL_MMAP_HEADER_OFFSET \
                           )

#define HPTR()    (tooldata_header_t*)( tool_mmap_base \
                                      + TOOL_MMAP_HEADER_OFFSET)

#define TPTR(idx) (CANON_TOOL_TABLE*)( tool_mmap_base \
                                     + TOOL_MMAP_HEADER_OFFSET \
                                     + TOOL_MMAP_HEADER_SIZE \
                                     + idx * TOOL_MMAP_STRIDE)
//---------------------------------------------------------------------
/* Note: emccfg.h defaults (seconds)
**       DEFAULT_EMC_TASK_CYCLE_TIME 0.100 (.001 common)
**       DEFAULT_EMC_IO_CYCLE_TIME   0.100
*/

static int tool_mmap_mutex_get()
{
    tooldata_header_t *hptr = HPTR();
    useconds_t waited_us  =      0;
    useconds_t delta_us   =    100;
    useconds_t maxwait_us = 10*1e6; //10seconds
    bool try_failed = 0;
    while ( rtapi_mutex_try(&(hptr->mutex)) ) { //true==failed
        usleep(delta_us); waited_us += delta_us;
        // fprintf(stderr,"!!!%8d UNEXPECTED: tool_mmap_mutex_get(): waited_us=%d\n"
        //        ,getpid(),waited_us);
        if (waited_us > maxwait_us) break;
    }
    if (waited_us > maxwait_us) {
        fprintf(stderr,"\n!!!%8d UNEXPECTED: tool_mmap_mutex_get(): FAIL\n",getpid());
        fprintf(stderr,"waited_us=%d delta_us=%d maxwait_us=%d\n\n",
                waited_us,delta_us,maxwait_us);
        rtapi_mutex_give(&(hptr->mutex)); // continue without mutex
        try_failed = 1;
    }
    if (try_failed) {return -1;}

    return 0;
} // tool_mmap_mutex_get()

static void tool_mmap_mutex_give()
{
    tooldata_header_t *hptr = HPTR();
    rtapi_mutex_give(&(hptr->mutex));
} // tool_mmap_mutex_give()

//typ creator: emc/ioControl.cc, sai/driver.cc
//    (first applicable process started in linuxcnc script)
int tool_mmap_creator(EMC_TOOL_STAT const * ptr,changer_t changer_method)
{
    static int inited=0;

    if (inited) {
        fprintf(stderr,"Error: tool_mmap_creator already called BYE\n");
        exit(EXIT_FAILURE);
    }
    toolstat = ptr; //note NULL for sai
    creator_fd = open(TOOL_MMAP_FILENAME,
                     TOOL_MMAP_CREATOR_OPEN_FLAGS,TOOL_MMAP_MODE);
    if (!creator_fd) {
        perror("tool_mmap_creator(): file open fail");
        exit(EXIT_FAILURE);
    }
    if (lseek(creator_fd, TOOL_MMAP_SIZE, SEEK_SET) == -1) {
        close(creator_fd);
        perror("tool_mmap_creator() lseek fail");
        exit(EXIT_FAILURE);
    }
    if (write(creator_fd, "\0", 1) < 0) {
        close(creator_fd);
        perror("tool_mmap_creator(): file tail write fail");
        exit(EXIT_FAILURE);
    }
    tool_mmap_base = (char*)mmap(0, TOOL_MMAP_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, creator_fd, 0);
    if (tool_mmap_base == MAP_FAILED) {
        close(creator_fd);
        perror("tool_mmap_creator(): mmap fail");
        exit(EXIT_FAILURE);
    }

    tooldata_header_t *hptr = HPTR();
    hptr->changer    = changer_method;
    hptr->last_index = 0;

    inited = 1;
    tool_mmap_mutex_give(); return 0;
} // tool_mmap_creator();

//typ: milltask, guis (emcmodule,emcsh,...), halui
int tool_mmap_user()
{
    int fd = open(TOOL_MMAP_FILENAME,
                  TOOL_MMAP_USER_OPEN_FLAGS, TOOL_MMAP_MODE);

    if (fd < 0) {
        perror("tool_mmap_user(): file open fail");
        exit(EXIT_FAILURE);
    }
    tool_mmap_base = (char*)mmap(0, TOOL_MMAP_SIZE, PROT_READ|PROT_WRITE,
                                 MAP_SHARED, fd, 0);

    if (tool_mmap_base == MAP_FAILED) {
        close(fd);
        perror("tool_mmap_user(): mmap fail");
        exit(EXIT_FAILURE);
    }
    return 0;
} //tool_mmap_user()

void tool_mmap_close()
{
    if (!tool_mmap_base) { return; }
    // mapped file is not deleted
    // flush mmapped file to filesystem
    if (msync(tool_mmap_base, TOOL_MMAP_SIZE, MS_SYNC) == -1) {
        perror("tool_mmap_close(): msync fail");
    }
    if (munmap(tool_mmap_base, TOOL_MMAP_SIZE) < 0) {
        close(creator_fd);
        perror("tool_mmap_close(): munmapfail");
        exit(EXIT_FAILURE);
    }
    close(creator_fd);
} //tool_mmap_close()

void tooldata_last_index_set(int idx)  //force last_index
{
    tool_mmap_mutex_get();
    tooldata_header_t *hptr = HPTR();
    if (idx < 0 || idx >= CANON_POCKETS_MAX) {
        fprintf(stderr,"!!!%8d PROBLEM: tooldata_last_index_set(): bad idx=%d\n",
               getpid(),idx);
        idx = 0;
        fprintf(stderr,"!!!continuing using idx=%d\n",idx);
    }
    hptr->last_index = idx;
    tool_mmap_mutex_give(); return;
} //tooldata_last_index_set()

int tooldata_last_index_get(void)
{
    tool_mmap_mutex_get();
    tooldata_header_t *hptr = HPTR();
    if (tool_mmap_base) {
        tool_mmap_mutex_give(); return hptr->last_index;
    } else {
        tool_mmap_mutex_give(); return -1;
    }
} // tooldata_last_index_get()

int tooldata_put(struct CANON_TOOL_TABLE tdata,int idx)
{
    if (!tool_mmap_base) {
        fprintf(stderr,"%8d tooldata_put() no tool_mmap_base BYE\n",getpid());
        exit(EXIT_FAILURE);
    }

    if (idx < 0 ||idx >= CANON_POCKETS_MAX) {
        fprintf(stderr,"!!!%8d PROBLEM: tooldata_put(): bad idx=%d, maxallowed=%d\n",
                getpid(),idx,CANON_POCKETS_MAX-1);
        idx = 0;
        fprintf(stderr,"!!!continuing using idx=%d\n",idx);
    }

    if (tool_mmap_mutex_get()) {
        fprintf(stderr,"!!!%8d PROBLEM: tooldata_put(): mutex get fail\n",getpid());
        fprintf(stderr,"!!!continuing without mutex\n");
    }

    tooldata_header_t *hptr = HPTR();
    if (idx > (int)(hptr->last_index) ) {  // extend known indices
        hptr->last_index = idx;
    }
    CANON_TOOL_TABLE *tptr = TPTR(idx);
    *tptr = tdata;

    if (toolstat) { //note sai does not use toolTableCurrent
       *(struct CANON_TOOL_TABLE*)(&toolstat->toolTableCurrent) = tdata;
    }
    tool_mmap_mutex_give(); return 0;
} // tooldata_put()

struct CANON_TOOL_TABLE tooldata_get(int idx)
{
    struct CANON_TOOL_TABLE tdata;

    if (!tool_mmap_base) {
        fprintf(stderr,"%8d tooldata_get() not mmapped BYE\n", getpid() );
        exit(EXIT_FAILURE);
    }
    if (idx >= CANON_POCKETS_MAX) {
        fprintf(stderr,"!!!%8d PROBLEM tooldata_get(): idx=%d, maxallowed=%d\n",
                getpid(),idx,CANON_POCKETS_MAX-1);
        idx = 0;
        fprintf(stderr,"!!!continuing using idx=%d\n",idx);
    }

    if (tool_mmap_mutex_get()) {
        fprintf(stderr,"!!!%8d UNEXPECTED: tool_mmap_mutex_get() fail\n",getpid());
        fprintf(stderr,"!!!continuing without mutex\n");
    }
    CANON_TOOL_TABLE *tptr = TPTR(idx);
    tdata =*tptr;

    tool_mmap_mutex_give(); return tdata;
} // tooldata_get()

int tooldata_find_index_for_tool(int toolno)
{
    tooldata_header_t *hptr = HPTR();
    tool_mmap_mutex_get();
    int idx;
    int idx_start;
    idx_start = 1;
    int foundidx = 0;  // 0 is result if not found

    if(hptr->changer == CHANGER_NONRANDOM  && toolno == 0) {
        tool_mmap_mutex_give(); return 0;
    }

    for (idx = idx_start; idx <= (int)hptr->last_index; idx++) { //note <=
        CANON_TOOL_TABLE *tptr = TPTR(idx);

        if (tptr->toolno == toolno) {
            foundidx = idx;
            break;
        }
    }
    tool_mmap_mutex_give(); return foundidx;
} // tooldata_find_index_for_tool()

// for error checking:
int tooldata_find_index_for_pocket(int pocketno)
{
    tooldata_header_t *hptr = HPTR();
    tool_mmap_mutex_get();
    int idx;
    int foundidx = -1;
    for (idx = 0; idx <= (int)hptr->last_index; idx++) { //note <=
        CANON_TOOL_TABLE *tptr = TPTR(idx);
        if (tptr->pocketno == pocketno) {
            foundidx = idx; // first one found
            break;
        }
    }
    tool_mmap_mutex_give(); return foundidx;
} // tooldata_find_index_for_pocket()

