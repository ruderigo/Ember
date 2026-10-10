/* Ember recorder: writes WAV files to recordings/ without blocking audio. */
#pragma once

void  rec_init(void);                       /* starts the background writer   */
void  rec_shutdown(void);                   /* finishes any open file         */
void  rec_push(const float *L, const float *R, int n);  /* audio thread    */
int   rec_start(void);                      /* session recording on           */
void  rec_stop(void);
int   rec_active(void);
float rec_seconds(void);
int   rec_save_loop(const float *L, const float *R, int len);  /* copies, saves in background */

/* last status message for the screen; *seq changes whenever it is updated */
const char *rec_message(unsigned *seq);
