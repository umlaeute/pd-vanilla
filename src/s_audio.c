/* Copyright (c) 2003, Miller Puckette and others.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/*  machine-independent (well, mostly!) audio layer.  Stores and recalls
    audio settings from argparse routine and from dialog window. 
*/

#include "m_pd.h"
#include "s_stuff.h"
#include <stdio.h>
#ifdef _WIN32
#include <time.h>
#else
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif /* _WIN32 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SYS_DEFAULTCH 2
typedef long t_pa_sample;
#define SYS_SAMPLEWIDTH sizeof(t_pa_sample)
#define SYS_BYTESPERCHAN (DEFDACBLKSIZE * SYS_SAMPLEWIDTH) 
#define SYS_XFERSAMPS (SYS_DEFAULTCH*DEFDACBLKSIZE)
#define SYS_XFERSIZE (SYS_SAMPLEWIDTH * SYS_XFERSAMPS)
#define MAXNDEV 20
#define DEVDESCSIZE 80

static void audio_getdevs(char *indevlist, int *nindevs,
    char *outdevlist, int *noutdevs, int *canmulti, int *cancallback, 
        int maxndev, int devdescsize);

    /* these are set in this file when opening audio, but then may be reduced,
    even to zero, in the system dependent open_audio routines. */
int sys_inchannels;
int sys_outchannels;
int sys_advance_samples;        /* scheduler advance in samples */
int sys_audioapi = API_DEFAULT;
int sys_audioapiopened = -1;    /* save last API opened for later closing */
static int sys_meters;          /* true if we're metering */
static t_sample sys_inmax;         /* max input amplitude */
static t_sample sys_outmax;        /* max output amplitude */

    /* exported variables */
int sys_schedadvance;   /* scheduler advance in microseconds */
t_float sys_dacsr;

t_sample *sys_soundout;
t_sample *sys_soundin;

    /* the "state" is normally one if we're open and zero otherwise; 
    but if the state is one, we still haven't necessarily opened the
    audio hardware; see audio_isopen() below. */
static int audio_state;

    /* last requested parameters */
static int audio_naudioindev = -1;
static int audio_audioindev[MAXAUDIOINDEV];
static int audio_audiochindev[MAXAUDIOINDEV];
static int audio_naudiooutdev = -1;
static int audio_audiooutdev[MAXAUDIOOUTDEV];
static int audio_audiochoutdev[MAXAUDIOOUTDEV];
static int audio_rate;
static int audio_advance = -1;
static int audio_callback;
static int audio_blocksize;

static int audio_callback_is_open;  /* reflects true actual state */
static int audio_nextinchans, audio_nextoutchans;
void sched_audio_callbackfn(void);
void sched_reopenmeplease(void);

static void sys_listaudiodevs(void );

/* generic audio api handling */
#include "s_media.h"
struct _audioapi {
  t_symbol*a_name;
  t_method a_init;
  t_audiofn_open a_open;
  t_audiofn_open_wcb a_callbackopen;
  t_audiofn_close a_close;
  t_audiofn_send a_send;
  t_audiofn_getdevs a_getdevs;
  t_method a_listdevs;

  struct _audioapi*next;
};


static void audioapi_void(void) {}

static t_audioapi*audioapis=NULL;
static t_audioapi*audioapi=NULL;

static t_audioapi*findapi(t_symbol*s) {
  t_audioapi*api=NULL;
  for(api=audioapis; NULL!=api; api=api->next) {
    if(api->a_name==s)
      return api;
  }
  return NULL;
}
static t_audioapi*setapi(t_symbol*s) {
  t_audioapi*api=findapi(s);
  audioapi=api;

  if(audioapi) {
    post("set api to %x: %s", audioapi, audioapi->a_name->s_name);
  }

  return audioapi;
}

static t_audioapi*getapi(t_symbol*s) {
  t_audioapi*api=NULL, *last=NULL;
  for(api=audioapis; NULL!=api; api=api->next) {
    if(api->a_name==s)
      return api;
    last=api;
  }
  
  api=(t_audioapi*)getbytes(sizeof(t_audioapi));
  memset(api, 0, sizeof(t_audioapi));
  api->a_name=s;

  if(NULL!=audioapis) {
    last->next=api;
  } else {
    audioapis=api;
  }

  return api;
}

t_audioapi*audioapi_new(t_symbol*name,
                        t_audiofn_open openfun, 
                        t_audiofn_close closefun,
                        t_audiofn_send sendfun
                        ) {
  t_audioapi*api=getapi(name);
  post("new api %x as '%s'", api, name->s_name);
  api->a_init=audioapi_void;
  api->a_open=openfun;
  api->a_close=closefun;
  api->a_send=sendfun;
  api->a_listdevs=sys_listaudiodevs;

  return api;
}


t_audioapi*audioapi_new_withcallback(t_symbol*name,
                                     t_audiofn_open_wcb openfun, 
                                     t_audiofn_close closefun,
                                     t_audiofn_send sendfun
                                     ) {
  t_audioapi*api=getapi(name);
  post("new cbapi %x as '%s'", api, name->s_name);
  api->a_init=audioapi_void;
  api->a_callbackopen=openfun;
  api->a_close=closefun;
  api->a_send=sendfun;
  api->a_listdevs=audioapi_void;

  return api;
}

void audioapi_addinit(t_audioapi*api, t_method fun) 
{
  if(api)api->a_init=fun;
}
void audioapi_addgetdevs(t_audioapi*api, t_audiofn_getdevs fun)
{
  if(api)api->a_getdevs=fun;
}
void audioapi_addlistdevs(t_audioapi*api, t_method fun)
{
  if(api)api->a_listdevs=fun;
}


void audioapi_init(void) 
{
  if(audioapi && audioapi->a_init)audioapi->a_init();
}
int audioapi_open(int nindev, int *indev, int nchin, int *chin, int noutdev, int *outdev, int nchout, int *chout, int rate)
{
  int result=1;
  if(audioapi) {
    if(audioapi->a_open){
      result=audioapi->a_open(nindev, indev, nchin, chin, noutdev, outdev, nchout, chout, rate);
      return result;
    }
    if(audioapi->a_callbackopen){
      int blksize = (sys_blocksize ? sys_blocksize : 64);

      result=audioapi->a_callbackopen(nindev, indev, nchin, chin, noutdev, outdev, nchout, chout, rate,
                                      0, 
                                      sys_soundin, sys_soundout, blksize, sys_advance_samples/blksize);
      return result;
    }

  }
  return 1;
}
int audioapi_callbackopen(int nindev, int *indev, int nchin, int *chin, int noutdev, int *outdev, int nchout, int *chout, int rate, 
                               t_audiocallback callback, 
                               t_sample *soundin, t_sample *soundout, int framesperbuf, int nbuffers)
{
  if(audioapi) {
    if(audioapi->a_callbackopen){
      return audioapi->a_callbackopen(nindev, indev, nchin, chin, noutdev, outdev, nchout, chout, rate,
                                      callback, 
                                      soundin, soundout, framesperbuf, nbuffers);
    }
    if(audioapi->a_open){
      return audioapi->a_open(nindev, indev, nchin, chin, noutdev, outdev, nchout, chout, rate);
    }
  }
  
  return 0;
}
void audioapi_close(void) 
{
  if(audioapi && audioapi->a_close)audioapi->a_close();
}
int audioapi_senddacs(void) 
{
  if(audioapi && audioapi->a_send)return(audioapi->a_send());
  return 0;
}
void audioapi_listdevs(void) 
{
  if(audioapi && audioapi->a_listdevs)audioapi->a_listdevs();
}
void audioapi_getdevs(char *indevlist, int *nindevs, char *outdevlist, int *noutdevs, int *canmulti, int*cancallback, int maxndev, int devdescsize) 
{
  int i;
  *nindevs = *noutdevs = 3;
  for (i = 0; i < 3; i++)
    {
      sprintf(indevlist + i * devdescsize, "input device #%d", i+1);
      sprintf(outdevlist + i * devdescsize, "output device #%d", i+1);
    }
  *canmulti = 0;
  *cancallback = 0;
  
  if(audioapi) {
    if(audioapi->a_getdevs)
      audioapi->a_getdevs(indevlist, nindevs, outdevlist, noutdevs, canmulti, maxndev, devdescsize);
    *cancallback=(NULL!=audioapi->a_callbackopen);
  }
}

void audioapi_register(void){
  post("audioapi register");
#ifdef USEAPI_OSS
  audioapi_oss();
#endif
#ifdef USEAPI_MMIO
  audioapi_mmio();
#endif
#ifdef USEAPI_ALSA
  post("audioapi register alsa");
  audioapi_alsa();
#endif
#ifdef USEAPI_PORTAUDIO
  audioapi_portaudio();
#endif
#ifdef USEAPI_JACK
  audioapi_jack();
#endif
#ifdef USEAPI_AUDIOUNIT
  audioapi_audiounit();
#endif
#ifdef USEAPI_ESD
  audioapi_esd();
#endif
}

/* generic audio api handling */

static int audio_isopen(void)
{
    return (audio_state &&
        ((audio_naudioindev > 0 && audio_audiochindev[0] > 0) 
            || (audio_naudiooutdev > 0 && audio_audiochoutdev[0] > 0)));
}

void sys_get_audio_params(
    int *pnaudioindev, int *paudioindev, int *chindev,
    int *pnaudiooutdev, int *paudiooutdev, int *choutdev,
    int *prate, int *padvance, int *pcallback, int *pblocksize)
{
    int i;
    *pnaudioindev = audio_naudioindev;
    for (i = 0; i < MAXAUDIOINDEV; i++)
        paudioindev[i] = audio_audioindev[i],
            chindev[i] = audio_audiochindev[i]; 
    *pnaudiooutdev = audio_naudiooutdev;
    for (i = 0; i < MAXAUDIOOUTDEV; i++)
        paudiooutdev[i] = audio_audiooutdev[i],
            choutdev[i] = audio_audiochoutdev[i]; 
    *prate = audio_rate;
    *padvance = audio_advance;
    *pcallback = audio_callback;
    *pblocksize = audio_blocksize;
}

void sys_save_audio_params(
    int naudioindev, int *audioindev, int *chindev,
    int naudiooutdev, int *audiooutdev, int *choutdev,
    int rate, int advance, int callback, int blocksize)
{
    int i;
    audio_naudioindev = naudioindev;
    for (i = 0; i < MAXAUDIOINDEV; i++)
        audio_audioindev[i] = audioindev[i],
            audio_audiochindev[i] = chindev[i]; 
    audio_naudiooutdev = naudiooutdev;
    for (i = 0; i < MAXAUDIOOUTDEV; i++)
        audio_audiooutdev[i] = audiooutdev[i],
            audio_audiochoutdev[i] = choutdev[i]; 
    audio_rate = rate;
    audio_advance = advance;
    audio_callback = callback;
    audio_blocksize = blocksize;
}

    /* init routines for any API which needs to set stuff up before
    any other API gets used.  This is only true of OSS so far. */
#ifdef USEAPI_OSS
void oss_init(void);
#endif

static void audio_init( void)
{
    static int initted = 0;
    if (initted)
        return;
    initted = 1;

    audioapi_init();


#ifdef USEAPI_OSS
    oss_init();
#endif
} 

    /* set channels and sample rate.  */

void sys_setchsr(int chin, int chout, int sr)
{
    int nblk;
    int inbytes = (chin ? chin : 2) *
                (DEFDACBLKSIZE*sizeof(t_sample));
    int outbytes = (chout ? chout : 2) *
                (DEFDACBLKSIZE*sizeof(t_sample));

    if (sys_soundin)
        freebytes(sys_soundin, 
            (sys_inchannels? sys_inchannels : 2) *
                (DEFDACBLKSIZE*sizeof(t_sample)));
    if (sys_soundout)
        freebytes(sys_soundout, 
            (sys_outchannels? sys_outchannels : 2) *
                (DEFDACBLKSIZE*sizeof(t_sample)));
    sys_inchannels = chin;
    sys_outchannels = chout;
    sys_dacsr = sr;
    sys_advance_samples = (sys_schedadvance * sys_dacsr) / (1000000.);
    if (sys_advance_samples < DEFDACBLKSIZE)
        sys_advance_samples = DEFDACBLKSIZE;

    sys_soundin = (t_sample *)getbytes(inbytes);
    memset(sys_soundin, 0, inbytes);

    sys_soundout = (t_sample *)getbytes(outbytes);
    memset(sys_soundout, 0, outbytes);

    if (sys_verbose)
        post("input channels = %d, output channels = %d",
            sys_inchannels, sys_outchannels);
    canvas_resume_dsp(canvas_suspend_dsp());
}

/* ----------------------- public routines ----------------------- */

    /* set audio device settings (after cleaning up the specified device and
    channel vectors).  The audio devices are "zero based" (i.e. "0" means the
    first one.)  We can later re-open audio and/or show the settings on a
    dialog window. */

void sys_set_audio_settings(int naudioindev, int *audioindev, int nchindev,
    int *chindev, int naudiooutdev, int *audiooutdev, int nchoutdev,
    int *choutdev, int rate, int advance, int callback, int blocksize)
{
    int i, *ip;
    int defaultchannels = SYS_DEFAULTCH;
    int inchans, outchans, nrealindev, nrealoutdev;
    int realindev[MAXAUDIOINDEV], realoutdev[MAXAUDIOOUTDEV];
    int realinchans[MAXAUDIOINDEV], realoutchans[MAXAUDIOOUTDEV];

    char indevlist[MAXNDEV*DEVDESCSIZE], outdevlist[MAXNDEV*DEVDESCSIZE];
    int indevs = 0, outdevs = 0, canmulti = 0, cancallback = 0;
    audio_getdevs(indevlist, &indevs, outdevlist, &outdevs, &canmulti,
        &cancallback, MAXNDEV, DEVDESCSIZE);

    if (rate < 1)
        rate = DEFAULTSRATE;
    if (advance < 0)
        advance = DEFAULTADVANCE;
    if (blocksize != (1 << ilog2(blocksize)) || blocksize < DEFDACBLKSIZE)
        blocksize = DEFDACBLKSIZE;
     audio_init();
        /* Since the channel vector might be longer than the
        audio device vector, or vice versa, we fill the shorter one
        in to match the longer one.  Also, if both are empty, we fill in
        one device (the default) and two channels. */ 
    if (naudioindev == -1)
    {           /* no input audio devices specified */
        if (nchindev == -1)
        {
            if (indevs >= 1)
            {
                nchindev=1;
                chindev[0] = defaultchannels;
                naudioindev = 1;
                audioindev[0] = DEFAULTAUDIODEV;
            }
            else naudioindev = nchindev = 0;
        }
        else
        {
            for (i = 0; i < MAXAUDIOINDEV; i++)
                audioindev[i] = i;
            naudioindev = nchindev;
        }
    }
    else
    {
        if (nchindev == -1)
        {
            nchindev = naudioindev;
            for (i = 0; i < naudioindev; i++)
                chindev[i] = defaultchannels;
        }
        else if (nchindev > naudioindev)
        {
            for (i = naudioindev; i < nchindev; i++)
            {
                if (i == 0)
                    audioindev[0] = DEFAULTAUDIODEV;
                else audioindev[i] = audioindev[i-1] + 1;
            }
            naudioindev = nchindev;
        }
        else if (nchindev < naudioindev)
        {
            for (i = nchindev; i < naudioindev; i++)
            {
                if (i == 0)
                    chindev[0] = defaultchannels;
                else chindev[i] = chindev[i-1];
            }
            naudioindev = nchindev;
        }
    }

    if (naudiooutdev == -1)
    {           /* not set */
        if (nchoutdev == -1)
        {
            if (outdevs >= 1)
            {
                nchoutdev=1;
                choutdev[0]=defaultchannels;
                naudiooutdev=1;
                audiooutdev[0] = DEFAULTAUDIODEV;
            }
            else nchoutdev = naudiooutdev = 0;
        }
        else
        {
            for (i = 0; i < MAXAUDIOOUTDEV; i++)
                audiooutdev[i] = i;
            naudiooutdev = nchoutdev;
        }
    }
    else
    {
        if (nchoutdev == -1)
        {
            nchoutdev = naudiooutdev;
            for (i = 0; i < naudiooutdev; i++)
                choutdev[i] = defaultchannels;
        }
        else if (nchoutdev > naudiooutdev)
        {
            for (i = naudiooutdev; i < nchoutdev; i++)
            {
                if (i == 0)
                    audiooutdev[0] = DEFAULTAUDIODEV;
                else audiooutdev[i] = audiooutdev[i-1] + 1;
            }
            naudiooutdev = nchoutdev;
        }
        else if (nchoutdev < naudiooutdev)
        {
            for (i = nchoutdev; i < naudiooutdev; i++)
            {
                if (i == 0)
                    choutdev[0] = defaultchannels;
                else choutdev[i] = choutdev[i-1];
            }
            naudiooutdev = nchoutdev;
        }
    }
    
        /* count total number of input and output channels */
    for (i = nrealindev = inchans = 0; i < naudioindev; i++)
        if (chindev[i] > 0)
    {
        realinchans[nrealindev] = chindev[i];
        realindev[nrealindev] = audioindev[i];
        inchans += chindev[i];
        nrealindev++;
    }
    for (i = nrealoutdev = outchans = 0; i < naudiooutdev; i++)
        if (choutdev[i] > 0)
    {
        realoutchans[nrealoutdev] = choutdev[i];
        realoutdev[nrealoutdev] = audiooutdev[i];
        outchans += choutdev[i];
        nrealoutdev++;
    }
    sys_schedadvance = advance * 1000;
    sys_log_error(ERR_NOTHING);
    audio_nextinchans = inchans;
    audio_nextoutchans = outchans;
    sys_setchsr(audio_nextinchans, audio_nextoutchans, rate);
    sys_save_audio_params(nrealindev, realindev, realinchans,
        nrealoutdev, realoutdev, realoutchans, rate, advance, callback,
            blocksize);
}

void sys_close_audio(void)
{
    if (sys_externalschedlib)
    {
        return;
    }
    if (!audio_isopen())
        return;

    audioapi_close();

    sys_inchannels = sys_outchannels = 0;
    sys_audioapiopened = -1;
    sched_set_using_audio(SCHED_AUDIO_NONE);
    audio_state = 0;
    audio_callback_is_open = 0;

    sys_vgui("set pd_whichapi 0\n");
}

    /* open audio using whatever parameters were last used */
void sys_reopen_audio( void)
{
    int naudioindev, audioindev[MAXAUDIOINDEV], chindev[MAXAUDIOINDEV];
    int naudiooutdev, audiooutdev[MAXAUDIOOUTDEV], choutdev[MAXAUDIOOUTDEV];
    int rate, advance, callback, blocksize, outcome = 0;
    sys_get_audio_params(&naudioindev, audioindev, chindev,
        &naudiooutdev, audiooutdev, choutdev, &rate, &advance, &callback,
            &blocksize);
    sys_setchsr(audio_nextinchans, audio_nextoutchans, rate);
    if (!naudioindev && !naudiooutdev)
    {
        sched_set_using_audio(SCHED_AUDIO_NONE);
        return;
    }

    if(callback) {
      int blksize = (sys_blocksize ? sys_blocksize : 64);
      outcome=audioapi_callbackopen(naudioindev,  audioindev,  naudioindev,  chindev,
                                    naudiooutdev, audiooutdev, naudiooutdev, choutdev, 
                                    rate,
                                    sched_audio_callbackfn,
                                    sys_soundin, sys_soundout, 
                                    blksize, sys_advance_samples/blksize
                                    );
    } else {
      outcome=audioapi_open(naudioindev,  audioindev,  naudioindev,  chindev,
                            naudiooutdev, audiooutdev, naudiooutdev, choutdev, 
                            rate);
    }

    if (outcome)    /* failed */
    {
        audio_state = 0;
        sched_set_using_audio(SCHED_AUDIO_NONE);
        sys_audioapiopened = -1;
        audio_callback_is_open = 0;
    }
    else
    {
      post("yey!");
                /* fprintf(stderr, "started w/callback %d\n", callback); */
        audio_state = 1;
        sched_set_using_audio(
            (callback ? SCHED_AUDIO_CALLBACK : SCHED_AUDIO_POLL));
        sys_audioapiopened = sys_audioapi;
        audio_callback_is_open = callback;
    }
    sys_vgui("set pd_whichapi %d\n",  (outcome == 0 ? sys_audioapi : 0));
}

int sys_send_dacs(void)
{
    if (sys_meters)
    {
        int i, n;
        t_sample maxsamp;
        for (i = 0, n = sys_inchannels * DEFDACBLKSIZE, maxsamp = sys_inmax;
            i < n; i++)
        {
            t_sample f = sys_soundin[i];
            if (f > maxsamp) maxsamp = f;
            else if (-f > maxsamp) maxsamp = -f;
        }
        sys_inmax = maxsamp;
        for (i = 0, n = sys_outchannels * DEFDACBLKSIZE, maxsamp = sys_outmax;
            i < n; i++)
        {
            t_sample f = sys_soundout[i];
            if (f > maxsamp) maxsamp = f;
            else if (-f > maxsamp) maxsamp = -f;
        }
        sys_outmax = maxsamp;
    }

    return audioapi_senddacs();
}

t_float sys_getsr(void)
{
     return (sys_dacsr);
}

int sys_get_outchannels(void)
{
     return (sys_outchannels); 
}

int sys_get_inchannels(void) 
{
     return (sys_inchannels);
}

void sys_getmeters(t_sample *inmax, t_sample *outmax)
{
    if (inmax)
    {
        sys_meters = 1;
        *inmax = sys_inmax;
        *outmax = sys_outmax;
    }
    else
        sys_meters = 0;
    sys_inmax = sys_outmax = 0;
}

void sys_reportidle(void)
{
}

static void audio_getdevs(char *indevlist, int *nindevs,
    char *outdevlist, int *noutdevs, int *canmulti, int *cancallback,
        int maxndev, int devdescsize)
{
    audio_init();
    audioapi_getdevs(indevlist, nindevs, outdevlist, noutdevs, canmulti, cancallback, maxndev, devdescsize);
}


static void sys_listaudiodevs(void )
{
    char indevlist[MAXNDEV*DEVDESCSIZE], outdevlist[MAXNDEV*DEVDESCSIZE];
    int nindevs = 0, noutdevs = 0, i, canmulti = 0, cancallback = 0;

    audio_getdevs(indevlist, &nindevs, outdevlist, &noutdevs, &canmulti,
        &cancallback, MAXNDEV, DEVDESCSIZE);

    if (!nindevs)
        post("no audio input devices found");
    else
    {
            /* To agree with command line flags, normally start at 1 */
            /* But microsoft "MMIO" device list starts at 0 (the "mapper"). */
            /* (see also sys_mmio variable in s_main.c)  */

        post("audio input devices:");
        for (i = 0; i < nindevs; i++)
            post("%d. %s", i + (sys_audioapi != API_MMIO),
                indevlist + i * DEVDESCSIZE);
    }
    if (!noutdevs)
        post("no audio output devices found");
    else
    {
        post("audio output devices:");
        for (i = 0; i < noutdevs; i++)
            post("%d. %s", i + (sys_audioapi != API_MMIO),
                outdevlist + i * DEVDESCSIZE);
    }
    post("API number %d\n", sys_audioapi);
}


    /* start an audio settings dialog window */
void glob_audio_properties(t_pd *dummy, t_floatarg flongform)
{
    char buf[1024 + 2 * MAXNDEV*(DEVDESCSIZE+4)];
        /* these are the devices you're using: */
    int naudioindev, audioindev[MAXAUDIOINDEV], chindev[MAXAUDIOINDEV];
    int naudiooutdev, audiooutdev[MAXAUDIOOUTDEV], choutdev[MAXAUDIOOUTDEV];
    int audioindev1, audioindev2, audioindev3, audioindev4,
        audioinchan1, audioinchan2, audioinchan3, audioinchan4,
        audiooutdev1, audiooutdev2, audiooutdev3, audiooutdev4,
        audiooutchan1, audiooutchan2, audiooutchan3, audiooutchan4;
    int rate, advance, callback, blocksize;
        /* these are all the devices on your system: */
    char indevlist[MAXNDEV*DEVDESCSIZE], outdevlist[MAXNDEV*DEVDESCSIZE];
    int nindevs = 0, noutdevs = 0, canmulti = 0, cancallback = 0, i;

    audio_getdevs(indevlist, &nindevs, outdevlist, &noutdevs, &canmulti,
         &cancallback, MAXNDEV, DEVDESCSIZE);

    sys_gui("global audio_indevlist; set audio_indevlist {}\n");
    for (i = 0; i < nindevs; i++)
        sys_vgui("lappend audio_indevlist {%s}\n",
            indevlist + i * DEVDESCSIZE);

    sys_gui("global audio_outdevlist; set audio_outdevlist {}\n");
    for (i = 0; i < noutdevs; i++)
        sys_vgui("lappend audio_outdevlist {%s}\n",
            outdevlist + i * DEVDESCSIZE);

    sys_get_audio_params(&naudioindev, audioindev, chindev,
        &naudiooutdev, audiooutdev, choutdev, &rate, &advance, &callback,
            &blocksize);

    /* post("naudioindev %d naudiooutdev %d longform %f",
            naudioindev, naudiooutdev, flongform); */
    if (naudioindev > 1 || naudiooutdev > 1)
        flongform = 1;

    audioindev1 = (naudioindev > 0 &&  audioindev[0]>= 0 ? audioindev[0] : 0);
    audioindev2 = (naudioindev > 1 &&  audioindev[1]>= 0 ? audioindev[1] : 0);
    audioindev3 = (naudioindev > 2 &&  audioindev[2]>= 0 ? audioindev[2] : 0);
    audioindev4 = (naudioindev > 3 &&  audioindev[3]>= 0 ? audioindev[3] : 0);
    audioinchan1 = (naudioindev > 0 ? chindev[0] : 0);
    audioinchan2 = (naudioindev > 1 ? chindev[1] : 0);
    audioinchan3 = (naudioindev > 2 ? chindev[2] : 0);
    audioinchan4 = (naudioindev > 3 ? chindev[3] : 0);
    audiooutdev1 = (naudiooutdev > 0 && audiooutdev[0]>=0 ? audiooutdev[0] : 0);  
    audiooutdev2 = (naudiooutdev > 1 && audiooutdev[1]>=0 ? audiooutdev[1] : 0);  
    audiooutdev3 = (naudiooutdev > 2 && audiooutdev[2]>=0 ? audiooutdev[2] : 0);  
    audiooutdev4 = (naudiooutdev > 3 && audiooutdev[3]>=0 ? audiooutdev[3] : 0);  
    audiooutchan1 = (naudiooutdev > 0 ? choutdev[0] : 0);
    audiooutchan2 = (naudiooutdev > 1 ? choutdev[1] : 0);
    audiooutchan3 = (naudiooutdev > 2 ? choutdev[2] : 0);
    audiooutchan4 = (naudiooutdev > 3 ? choutdev[3] : 0);
    sprintf(buf,
"pdtk_audio_dialog %%s \
%d %d %d %d %d %d %d %d \
%d %d %d %d %d %d %d %d \
%d %d %d %d %d %d\n",
        audioindev1, audioindev2, audioindev3, audioindev4, 
        audioinchan1, audioinchan2, audioinchan3, audioinchan4, 
        audiooutdev1, audiooutdev2, audiooutdev3, audiooutdev4,
        audiooutchan1, audiooutchan2, audiooutchan3, audiooutchan4, 
        rate, advance, canmulti, (cancallback ? callback : -1),
        (flongform != 0), blocksize);
    gfxstub_deleteforkey(0);
    gfxstub_new(&glob_pdobject, (void *)glob_audio_properties, buf);
}

extern int pa_foo;
    /* new values from dialog window */
void glob_audio_dialog(t_pd *dummy, t_symbol *s, int argc, t_atom *argv)
{
    int naudioindev, audioindev[MAXAUDIOINDEV], chindev[MAXAUDIOINDEV];
    int naudiooutdev, audiooutdev[MAXAUDIOOUTDEV], choutdev[MAXAUDIOOUTDEV];
    int rate, advance, audioon, i, nindev, noutdev;
    int audioindev1, audioinchan1, audiooutdev1, audiooutchan1;
    int newaudioindev[4], newaudioinchan[4],
        newaudiooutdev[4], newaudiooutchan[4];
        /* the new values the dialog came back with: */
    int newrate = atom_getintarg(16, argc, argv);
    int newadvance = atom_getintarg(17, argc, argv);
    int newcallback = atom_getintarg(18, argc, argv);
    int newblocksize = atom_getintarg(19, argc, argv);

    for (i = 0; i < 4; i++)
    {
        newaudioindev[i] = atom_getintarg(i, argc, argv);
        newaudioinchan[i] = atom_getintarg(i+4, argc, argv);
        newaudiooutdev[i] = atom_getintarg(i+8, argc, argv);
        newaudiooutchan[i] = atom_getintarg(i+12, argc, argv);
    }

    for (i = 0, nindev = 0; i < 4; i++)
    {
        if (newaudioinchan[i])
        {
            newaudioindev[nindev] = newaudioindev[i];
            newaudioinchan[nindev] = newaudioinchan[i];
            /* post("in %d %d %d", nindev,
                newaudioindev[nindev] , newaudioinchan[nindev]); */
            nindev++;
        }
    }
    for (i = 0, noutdev = 0; i < 4; i++)
    {
        if (newaudiooutchan[i])
        {
            newaudiooutdev[noutdev] = newaudiooutdev[i];
            newaudiooutchan[noutdev] = newaudiooutchan[i];
            /* post("out %d %d %d", noutdev,
                newaudiooutdev[noutdev] , newaudioinchan[noutdev]); */
            noutdev++;
        }
    }
    
    sys_set_audio_settings_reopen(nindev, newaudioindev, nindev, newaudioinchan,
        noutdev, newaudiooutdev, noutdev, newaudiooutchan,
        newrate, newadvance, newcallback, newblocksize);
}

void sys_set_audio_settings_reopen(int naudioindev, int *audioindev, int nchindev,
    int *chindev, int naudiooutdev, int *audiooutdev, int nchoutdev,
    int *choutdev, int rate, int advance, int callback, int newblocksize)
{
    if (callback < 0)
        callback = 0;
    if (newblocksize != (1<<ilog2(newblocksize)) ||
        newblocksize < DEFDACBLKSIZE || newblocksize > 2048)
            newblocksize = DEFDACBLKSIZE;
    
    if (!audio_callback_is_open && !callback)
        sys_close_audio();
    sys_set_audio_settings(naudioindev, audioindev, nchindev, chindev,
        naudiooutdev, audiooutdev, nchoutdev, choutdev,
        rate, advance, (callback >= 0 ? callback : 0), newblocksize);
    if (!audio_callback_is_open && !callback)
        sys_reopen_audio();
    else sched_reopenmeplease();
}

void sys_listdevs(void )
{
  audioapi_listdevs();
  sys_listmididevs();
}

void sys_get_audio_devs(char *indevlist, int *nindevs,
    char *outdevlist, int *noutdevs, int *canmulti, int *cancallback, 
                        int maxndev, int devdescsize)
{
  audio_getdevs(indevlist, nindevs,
                outdevlist, noutdevs, 
                canmulti, cancallback, 
                maxndev, devdescsize);
}

void sys_set_audio_api(int which)
{
     switch(which) {
     case API_ALSA: 
       setapi(gensym("ALSA")); 
       break;
     case API_OSS:
       setapi(gensym("OSS")); 
       break;
     case API_MMIO:
       setapi(gensym("MMIO")); 
       break;
     case API_PORTAUDIO:
       setapi(gensym("portaudio")); 
       break;
     case API_JACK:
       setapi(gensym("jack")); 
       break;
     case API_AUDIOUNIT:
       setapi(gensym("AudioUnit")); 
       break;
     case API_ESD:
       setapi(gensym("ESD")); 
       break;

     case API_NONE: 
     case API_SGI:
       break;
     }

     sys_audioapi = which;

     if (sys_verbose)
        post("sys_audioapi %d", sys_audioapi);
}

void glob_audio_setapi(void *dummy, t_floatarg f)
{
    int newapi = f;
    if (newapi)
    {
        if (newapi == sys_audioapi)
        {
            if (!audio_isopen())
                sys_reopen_audio();
        }
        else
        {
            sys_close_audio();
            sys_set_audio_api(newapi);
                /* bash device params back to default */
            audio_naudioindev = audio_naudiooutdev = 1;
            audio_audioindev[0] = audio_audiooutdev[0] = DEFAULTAUDIODEV;
            audio_audiochindev[0] = audio_audiochoutdev[0] = SYS_DEFAULTCH;
            sys_reopen_audio();
        }
        glob_audio_properties(0, 0);
    }
    else if (audio_isopen())
    {
        sys_close_audio();
    }
}

    /* start or stop the audio hardware */
void sys_set_audio_state(int onoff)
{
    if (onoff)  /* start */
    {
        if (!audio_isopen())
            sys_reopen_audio();    
    }
    else
    {
        if (audio_isopen())
            sys_close_audio();
    }
}

void sys_get_audio_apis(char *buf)
{
    int n = 0;

    audioapi_register();

    /* LATER get rid of the API_??? enumeration and only use symbolic names for api selection
     * then iterate through all available APIs rather than explicitely naming them here 
     */

    strcpy(buf, "{ ");
    if(NULL!=findapi(gensym("OSS")))
      sprintf(buf + strlen(buf), "{OSS %d} ", API_OSS); n++;
    if(NULL!=findapi(gensym("MMIO")))
      sprintf(buf + strlen(buf), "{\"standard (MMIO)\" %d} ", API_MMIO); n++;
    if(NULL!=findapi(gensym("ALSA")))
      sprintf(buf + strlen(buf), "{ALSA %d} ", API_ALSA); n++;
    if(NULL!=findapi(gensym("portaudio")))
      {
#ifdef _WIN32
        sprintf(buf + strlen(buf),
                "{\"ASIO (via portaudio)\" %d} ", API_PORTAUDIO);
#elif defined OSX
        sprintf(buf + strlen(buf),
                "{\"standard (portaudio)\" %d} ", API_PORTAUDIO);
#else
        sprintf(buf + strlen(buf), "{portaudio %d} ", API_PORTAUDIO);
#endif
        n++;
      }
    if(NULL!=findapi(gensym("jack")))
      sprintf(buf + strlen(buf), "{jack %d} ", API_JACK); n++;
    if(NULL!=findapi(gensym("AudioUnit")))
      sprintf(buf + strlen(buf), "{AudioUnit %d} ", API_AUDIOUNIT); n++;
    if(NULL!=findapi(gensym("ESD")))
      sprintf(buf + strlen(buf), "{ESD %d} ", API_ESD); n++;

    strcat(buf, "}");
        /* then again, if only one API (or none) we don't offer any choice. */
    if (n < 2)
        strcpy(buf, "{}");
}

#ifdef USEAPI_ALSA
void alsa_putzeros(int n);
void alsa_getzeros(int n);
void alsa_printstate( void);
#endif

    /* debugging */
void glob_foo(void *dummy, t_symbol *s, int argc, t_atom *argv)
{
    t_symbol *arg = atom_getsymbolarg(0, argc, argv);
    if (arg == gensym("restart"))
        sys_reopen_audio();
#ifdef USEAPI_ALSA
    else if (arg == gensym("alsawrite"))
    {
        int n = atom_getintarg(1, argc, argv);
        alsa_putzeros(n);
    }
    else if (arg == gensym("alsaread"))
    {
        int n = atom_getintarg(1, argc, argv);
        alsa_getzeros(n);
    }
    else if (arg == gensym("print"))
    {
        alsa_printstate();
    }
#endif
}
