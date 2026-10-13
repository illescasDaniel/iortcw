// cl_cin_avi.h -- MJPEG/AVI cinematic reader (HD video remaster path).
//
// The HD video pack ships video/<name>.avi next to the original RoQ. CIN_PlayCinematic
// prefers the AVI when present, so existing callers keep asking for "<name>.roq".

#ifndef CL_CIN_AVI_H
#define CL_CIN_AVI_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

typedef struct cinAvi_s cinAvi_t;

typedef enum {
	CIN_AVI_VIDEO,      // a frame was decoded into CIN_AVI_Frame()
	CIN_AVI_AUDIO,      // 16-bit PCM in CIN_AVI_Audio()
	CIN_AVI_END,
	CIN_AVI_ERROR
} cinAviChunk_t;

// Opens video/<x>.avi. Returns NULL when missing or not a usable MJPEG/AVI (power-of-two size).
cinAvi_t *CIN_AVI_Open( const char *name );
void CIN_AVI_Close( cinAvi_t *a );
void CIN_AVI_Rewind( cinAvi_t *a );

// Reads the next chunk; video chunks are decoded to RGBA.
cinAviChunk_t CIN_AVI_Next( cinAvi_t *a );

int CIN_AVI_Width( const cinAvi_t *a );
int CIN_AVI_Height( const cinAvi_t *a );
int CIN_AVI_FPS( const cinAvi_t *a );
byte *CIN_AVI_Frame( cinAvi_t *a );
// Valid after CIN_AVI_Next returned CIN_AVI_AUDIO: *samples = frames per channel.
const byte *CIN_AVI_Audio( const cinAvi_t *a, int *samples, int *rate, int *channels );

#endif
