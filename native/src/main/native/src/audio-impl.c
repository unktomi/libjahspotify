/*
 * Copyright (c) 2010 Spotify Ltd
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
 *
 *
 * This file is part of the libspotify examples suite.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

#include "audio.h"
#include "Logging.h"

#define NUM_BUFFERS 3
static void error_exit(const char *msg)
{
    puts(msg);
    exit(1);
}



void audio_close()
{
    
}

float get_audio_gain()
{
  return 0;
}

void set_audio_gain(float gain)
{
}


static void* audio_start(void *aux)
{
}


void audio_init(audio_fifo_t *af)
{
    pthread_t tid;

    TAILQ_INIT(&af->q);
    af->qlen = 0;

    pthread_mutex_init(&af->mutex, NULL);
    pthread_cond_init(&af->cond, NULL);

    pthread_create(&tid, NULL, audio_start, af);
}

void audio_fifo_flush(audio_fifo_t *af)
{
    audio_fifo_data_t *afd;

    pthread_mutex_lock(&af->mutex);

    while ((afd = TAILQ_FIRST(&af->q))) {
        TAILQ_REMOVE(&af->q, afd, link);
        free(afd);
    }

    af->qlen = 0;
    pthread_mutex_unlock(&af->mutex);
}
