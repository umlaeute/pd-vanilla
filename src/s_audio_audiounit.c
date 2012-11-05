
/* ------------- routines for Apple AudioUnit in AudioToolbox -------------- */
#ifdef USEAPI_AUDIOUNIT

/* this is currently a placeholder file while we decide which one of three implementations of this API we use */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m_pd.h"
#include "s_stuff.h"
#include "s_media.h"
#include <AudioToolbox/AudioToolbox.h>

pthread_mutex_t audiounit_mutex;
pthread_cond_t audiounit_sem;

int audiounit_open_audio(int naudioindev, int *audioindev, int nchindev,
    int *chindev, int naudiooutdev, int *audiooutdev, int nchoutdev,
    int *choutdev, int rate, int blocksize)
{
    return 0;
}

void audiounit_close_audio(void) 
{
}

int audiounit_send_dacs(void)
{
    return 0;
}

void audiounit_getdevs(char *indevlist, int *nindevs,
    char *outdevlist, int *noutdevs, int *canmulti, 
        int maxndev, int devdescsize)
{
    post("device getting not implemented for AudioUnit yet\n");
}

void audiounit_listdevs( void)
{
    post("device listing not implemented for AudioUnit yet\n");
}
#endif /* AUDIOUNIT */


void audioapi_audiounit(void) {
#ifdef USEAPI_AUDIOUNIT
  t_audioapi*api=audioapi_new(gensym("AudioUnit"),
                              audiounit_open_audio,
                              audiounit_close_audio,
                              audiounit_send_dacs);
  if(NULL==api)return;
  audioapi_addgetdevs(api, audiounit_getdevs);
  audioapi_addlistdevs(api, audiounit_listdevs);
#endif /* AudioUnit */
}
