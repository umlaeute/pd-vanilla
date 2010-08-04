/* Copyright (c) 1997-1999 Miller Puckette.
   Copyright (c) 2010 IOhannes m zmölnig

* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* This file contains declarations for media API support */

#ifndef __s_media_h
#define __s_media_h
#include "m_pd.h"

EXTERN_STRUCT _audioapi;
#define t_audioapi struct _audioapi

/*
 OPEN:
 * jack:  (int inchans, int outchans, int rate,     t_audiocallback callback)
 *   pa:  (int inchans, int outchans, int rate,     t_sample *soundin, t_sample *soundout, int framesperbuf, int nbuffers, int indeviceno, int outdeviceno, t_audiocallback callbackfn)
 *  oss:  (int nindev,  int *indev,   int nchin,    int *chin, int noutdev, int *outdev, int nchout, int *chout, int rate)
 * alsa:  (int nindev,  int *indev,   int nchin,    int *chin, int noutdev, int *outdev, int nchout, int *chout, int rate)
 *  esd:  (int nindev,  int *indev,   int nchin,    int *chin, int noutdev, int *outdev, int nchout, int *chout, int rate)
 * mmio:  (int nindev,  int *indev,   int nchin,    int *chin, int noutdev, int *outdev, int nchout, int *chout, int rate)

OSS  (naudioindev, audioindev, naudioindev, chindev, naudiooutdev, audiooutdev, naudiooutdev, choutdev, rate);
ALSA (naudioindev, audioindev, naudioindev, chindev, naudiooutdev, audiooutdev, naudiooutdev, choutdev, rate);
MMIO (naudioindev, audioindev, naudioindev, chindev, naudiooutdev, audiooutdev, naudiooutdev, choutdev, rate);
ESD  (naudioindev, audioindev, naudioindev, chindev, naudiooutdev, audiooutdev, naudiooutdev, choutdev, rate);

PA   ((naudioindev  > 0 ? chindev[0]  : 0),   .
      (naudiooutdev > 0 ? choutdev[0] : 0),   .
      rate,                                   .
      sys_soundin,                            (t_sample*soundin)
      sys_soundout,                           (t_sample*soundout)
      blksize,                                (int framesperbuf)
      sys_advance_samples/blksize,            (int nbuffers)
      (naudioindev  > 0 ? audioindev[0]  : 0), .
      (naudiooutdev > 0 ? audiooutdev[0] : 0), .
      (callback ? sched_audio_callbackfn : 0));

JACK ((naudioindev > 0 ? chindev[0]  : 0), .
      (naudioindev > 0 ? choutdev[0] : 0), .
      rate,                                .
      (callback ? sched_audio_callbackfn : 0));

AU   ((naudioindev > 0 ? chindev[0] : 0), 
      (naudioindev > 0 ? choutdev[0] : 0), 
      rate);



 CLOSE, SEND_DACS, LISTDEVS: (void)

 GETDEVS:
 * jack:  (char *indevlist, int *nindevs, char *outdevlist, int *noutdevs, int *canmulti, int maxndev, int devdescsize)
 *   pa:  (char *indevlist, int *nindevs, char *outdevlist, int *noutdevs, int *canmulti, int maxndev, int devdescsize)
 *  oss:  (char *indevlist, int *nindevs, char *outdevlist, int *noutdevs, int *canmulti, int maxndev, int devdescsize)
 * alsa:  (char *indevlist, int *nindevs, char *outdevlist, int *noutdevs, int *canmulti, int maxndev, int devdescsize)
 *  esd:  (char *indevlist, int *nindevs, char *outdevlist, int *noutdevs, int *canmulti, int maxndev, int devdescsize)
 * mmio:  (char *indevlist, int *nindevs, char *outdevlist, int *noutdevs, int *canmulti, int maxndev, int devdescsize)

*/

typedef int (*t_audiofn_open)       (int nindev, int *indev, int nchin, int *chin, int noutdev, int *outdev, int nchout, int *chout, int rate);
typedef int (*t_audiofn_open_wcb)   (int nindev, int *indev, int nchin, int *chin, int noutdev, int *outdev, int nchout, int *chout, int rate,
                                      t_audiocallback callback, 
                                      t_sample *soundin, t_sample *soundout, int framesperbuf, int nbuffers);

typedef void(*t_audiofn_close)       (void);
typedef int (*t_audiofn_send)        (void);

typedef void (*t_audiofn_getdevs)(char *indevlist, int *nindevs, char *outdevlist, int *noutdevs, int *canmulti, int maxndev, int devdescsize);


EXTERN t_audioapi*audioapi_new(t_symbol*name,
                               t_audiofn_open openfun, 
                               t_audiofn_close closefun,
                               t_audiofn_send sendfun
                               );

EXTERN t_audioapi*audioapi_new_withcallback(t_symbol*name,
                                            t_audiofn_open_wcb openfun,
                                            t_audiofn_close closefun,
                                            t_audiofn_send sendfun
                                            );

EXTERN void audioapi_addinit(t_audioapi*api,     t_method fun);
EXTERN void audioapi_addgetdevs(t_audioapi*api,  t_audiofn_getdevs fun);
EXTERN void audioapi_addlistdevs(t_audioapi*api, t_method fun);



#endif /* __s_media_h */
