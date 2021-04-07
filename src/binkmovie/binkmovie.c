//
// Bink video player interface that uses the games drawer.
//
// Due to the use of the older version of the Bink32.dll, make sure
// any .BIK video you produce sets the "Compress level" to the desired
// compression level +100. Example; A level of 4 should be 104, this makes
// the encoder produce the BIK video file with the older audio format
// which is required by the older DLL. Otherwise you will not hear any
// audio when you video is played back by the game.
//
// Author: CCHyper
//

#include "binkmovie.h"
#include "bink.h"
#include "bink_load_dll.h"
#include <assert.h>


BOOL BinkBreakoutAllowed = TRUE;
BOOL BinkScaleToFit = FALSE;


// Master playback volume.
static float BinkMasterVolume = 0.7f;

static HBINK BinkHandle;
static int SurfaceFlags;
static DSurface * BinkVideoSurface;
static RECT VideoRect;
static HANDLE FileHandle;
static BOOL IsPlaying;
static BOOL NewFrame;
static int LastFrameNum;


BOOL Audio_Available()
{
	return !Debug_Quiet && DSAudio_SoundObject && !DSAudio_AudioDone;
}


LPDIRECTSOUND Audio_SoundObject()
{
	return DSAudio_SoundObject;
}


void __fastcall BinkMovie_Close(void)
{
    if (BinkHandle) {
        BinkClose(BinkHandle);
		BinkHandle = 0;
    }
    if (FileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(FileHandle);
        FileHandle = INVALID_HANDLE_VALUE;
    }
	
    BinkVideoSurface = NULL;
}


void __fastcall BinkMovie_SetPosition(unsigned x_pos, unsigned y_pos)
{
    WWDebug_Printf("BinkMovie_SetPosition()\n");

    RECT surface_rect;
	surface_rect.left = 0;
	surface_rect.top = 0;
	surface_rect.right = BinkVideoSurface->Width;
    surface_rect.bottom = BinkVideoSurface->Height;

    RECT bink_rect;
	bink_rect.left = x_pos;
	bink_rect.top = y_pos;
	bink_rect.right = BinkHandle->Width;
	bink_rect.bottom = BinkHandle->Height;

    VideoRect = Rect_Intersect(&surface_rect, &bink_rect, NULL, NULL);
}


void __fastcall BinkMovie_Go_To_Frame(int frame)
{
    BinkGoto(BinkHandle, frame, BINKGOTOQUICK);
    LastFrameNum = 0;
}


void __fastcall BinkMovie_Pause(BOOL pause)
{
    BinkPause(BinkHandle, (pause & 0xFF));
}


BOOL __fastcall BinkMovie_Has_Frames_Left(void)
{
    return BinkHandle->FrameNum >= BinkHandle->Frames || BinkHandle->FrameNum < LastFrameNum;
}


BOOL __fastcall BinkMovie_Open(char * filename)
{
    BinkMovie_Close();

    LastFrameNum = 0;

    //
    // Tell Bink to use DirectSound (must be before BinkOpen)!
    //
    if (Audio_Available()) {
        BinkSoundUseDirectSound((BINKOPENDIRECTSOUND)Audio_SoundObject());
    } else {
		WWDebug_Printf("BinkMovie_Open() - Audio playback not available.\n");
	}
    
    //
    // Open a handle to the file if it exists locally.
    //
	RawFileClass file;
	RawFileClass__RawFileClass(&file, filename);
    if (RawFileClass__Is_Available(&file, FALSE)) {
        BinkHandle = BinkOpen(filename, 0);

    } else {
        
        //
        // So the file was not found locally, try finding it within a mixfile.
        //
        long start = 0;
        MixFileClass *mixfile = NULL;
        if (MixFileClass__Offset(filename, NULL, &mixfile, &start, NULL)) {

            FileHandle = CreateFileA(mixfile->Filename, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (FileHandle != INVALID_HANDLE_VALUE) {
                
                //
                // Set the file handle pointer to the offset within the mixfile.
                //
                SetFilePointer(FileHandle, start, NULL, FILE_BEGIN);

                //
                // Now open the handle. When we pass "BINKFILEHANDLE" into BinkOpen, it tells the
                // Bink playback engine that the first param is in fact a Windows file handle and
                // not the actual filename.
                //
                BinkHandle = BinkOpen((char *)FileHandle, BINKFILEHANDLE);
            }
        }
    }
	
	if (BinkHandle->Width > GameOptionsClass_ScreenWidth || BinkHandle->Height > GameOptionsClass_ScreenHeight) {
		WWDebug_Printf("Video can not be played due to low user resolution!\n");
		ShowCursor(TRUE);
		char buffer[256];
		sprintf(buffer,
			"Video \"%s\" can not be played due to low resolution size, please\n"
			"increase your resolution to at least %dx%d to play this video.\n",
			filename, BinkHandle->Width, BinkHandle->Height);
		MessageBoxA(MainWindow, buffer, "Error!", MB_ICONWARNING|MB_OK);
		BinkMovie_Close();
#ifdef BINK_REQUIRED
		Emergency_Exit();
		exit(1);
#else
		ShowCursor(FALSE);
#endif
		return FALSE;
	}

    if (BinkHandle) {

        //
        // Adjust playback volume based on the set user volume.
        //
        if (GameOptionsClass_VoiceVolume != BinkMasterVolume) {
            BinkMasterVolume = GameOptionsClass_VoiceVolume;
            BinkSetVolume(BinkHandle, (BinkMasterVolume * 32768.0f));
        }

        RECT rect;
        int x = 0;
        int y = 0;

        if (!BinkVideoSurface) {
			BinkVideoSurface = PrimarySurface;
		}
		
		//
		// Center video in the main window.
		//
		GetClientRect(MainWindow, &rect);
		x = (rect.right - rect.left - BinkHandle->Width) / 2;
		y = (rect.bottom - rect.top - BinkHandle->Height) / 2;

        BinkMovie_SetPosition(x, y);

        //
        // Store a copy of the surface flags.
        //
        SurfaceFlags = BinkDDSurfaceType((void *)PrimarySurface->VideoSurfacePtr);

        return TRUE;
    }

    WWDebug_Printf("BinkMovie_Open() - Bink Error: %s\n", BinkGetError());

    return FALSE;
}


BOOL __fastcall BinkMovie_Next_Frame(DSurface * surface, unsigned x_pos, unsigned y_pos)
{
    if (GameOptionsClass_VoiceVolume != BinkMasterVolume) {
        BinkMasterVolume = GameOptionsClass_VoiceVolume;
        BinkSetVolume(BinkHandle, (BinkMasterVolume * 32768.0f));
    }

    BinkMovie_ResumePause();

    int result = BinkWait(BinkHandle);

    //
    // Do we have a new frame to draw?
    //
    if (NewFrame || !result) {

        //
        // Check to see if a frame is ready to be drawn.
        //
        while (!BinkWait(BinkHandle)) {

            //
            // Start decompressing the next frame.
            //
            BinkDoFrame(BinkHandle);

            NewFrame = FALSE;

            BinkMovie_Render_Frame(surface, x_pos, y_pos);

            LastFrameNum = BinkHandle->FrameNum;

            //
            // Next frame, please.
            //
            BinkNextFrame(BinkHandle);
        }

        return TRUE;
    }

    return result;
}


BOOL __fastcall BinkMovie_Advance_Frame(void)
{
    return BinkMovie_Next_Frame(BinkVideoSurface, VideoRect.left, VideoRect.top);
}


void __fastcall BinkMovie_Play(void)
{
    if (!BinkHandle) {
        WWDebug_Printf("BinkMovie_Play() - Bink handle is null! Bink Error: %s\n", BinkGetError());
        return;
    }

    WWKeyboardClass__Clear(WWKeyboard);

    IsPlaying = TRUE;
    NewFrame = FALSE;

    for (;;) {
    
        if (BinkMovie_Has_Frames_Left()) {
            break;
        }

        //
        // Are there any messages to handle?
        //
        if (!VQA_Windows_Message_Loop()) {
            break;

        } else {

            //
            // If paused, we don't need to redraw every tick, so add a little wait to take
            // the stress away from the CPU, because you know, it has a hard life...
            //
            if (!IsPlaying) {
                Sleep(33); // Sleep for 33 msec.
            }

            //
            // Draw the next frame.
            //
            if (BinkVideoSurface == PrimarySurface) {

                RECT rect;
                GetClientRect(MainWindow, &rect);
                ClientToScreen(MainWindow, (LPPOINT)&rect);

                BinkMovie_Next_Frame(BinkVideoSurface, rect.left + VideoRect.left, rect.top + VideoRect.top);

            } else if (BinkMovie_Next_Frame(BinkVideoSurface, VideoRect.left, VideoRect.top)) {

                RECT rect;
                GetClientRect(MainWindow, &rect);
                ClientToScreen(MainWindow, (LPPOINT)&rect);

                RECT dest_rect = VideoRect;
                dest_rect.left += rect.left;
                dest_rect.top += rect.top;
                PrimarySurface->vtable->BlitPart(PrimarySurface, &dest_rect, BinkVideoSurface, &VideoRect, false, true);
            }

            //
            // Check if the Esc key has been pressed. If so, break out and stop all
            // frame updates.
            //
			if (BinkBreakoutAllowed) {
				if (WWKeyboardClass__Check(WWKeyboard) && WWKeyboardClass__Get(WWKeyboard) == (KN_RLSE_BIT|KN_ESC)) {
					WWDebug_Printf("BinkMovie_Callback() - Breakout.\n");
					break;
				}
			}

        }
    
    }

    WWKeyboardClass__Clear(WWKeyboard);
}


void __fastcall BinkMovie_Draw_Frame(void)
{
    if (BinkVideoSurface == PrimarySurface) {
        RECT rect;
        GetClientRect(MainWindow, &rect);
        ClientToScreen(MainWindow, (LPPOINT)&rect);
        BinkMovie_Render_Frame(BinkVideoSurface, rect.left + VideoRect.left, rect.top + VideoRect.top);
    } else {
        BinkMovie_Render_Frame(BinkVideoSurface, VideoRect.left, VideoRect.top);
    }
}


void __fastcall BinkMovie_Render_Frame(DSurface * surface, unsigned x_pos, unsigned y_pos)
{
    if (!surface) {
        WWDebug_Printf("BinkMovie_Render_Frame() - Surface is null!\n");
        return;
    }

    //
    // Lock the surface so that we can copy the decompressed frame into it.
    //
    void *buffptr = surface->vtable->Lock(surface, 0, 0);
    if (buffptr) {
		
		if (BinkScaleToFit) {
			
			//
			// Copy the decompressed frame into the buffer surface, then scale copy to the primary surface.
			//
			
			// TODO!
			
			//BSurface buffsurface(BinkHandle->Width, BinkHandle-Height, surface->vtable->Get_Pitch(surface));
			//BinkCopyToBuffer(BinkHandle, buffptr, buffsurface.vtable->Get_Pitch(surface), buffsurface->Height, x_pos, y_pos, SurfaceFlags|BINKCOPYALL);
			
			//PrimarySurface->vtable->BlitPart(buffsurface);
			
		} else {
			
			//
			// Copy the decompressed frame into the surface buffer (this might be currently on-screen).
			//
			BinkCopyToBuffer(BinkHandle, buffptr, surface->vtable->Get_Pitch(surface), surface->Height, x_pos, y_pos, SurfaceFlags|BINKCOPYALL);
			
		}

        //
        // Finished, now unlock the BinkBuffer.
        //
        surface->vtable->Unlock(surface);
    }
}


BOOL __fastcall BinkMovie_Create(char * filename)
{
	if (!BinkImportsLoaded) {
		return FALSE;
	}	
	
    BinkHandle = 0;
    SurfaceFlags = 0;
    BinkVideoSurface = NULL;
    BinkVideoSurface = PrimarySurface;
    VideoRect.left = 0;
    VideoRect.top = 0;
    VideoRect.right = 0;
    VideoRect.bottom = 0;
    FileHandle = INVALID_HANDLE_VALUE;
    IsPlaying = TRUE;
    NewFrame = FALSE;
    LastFrameNum = 0;

    return BinkMovie_Open(filename);
}


BOOL __fastcall BinkMovie_CreateSurface(char * filename, DSurface *surface)
{
	if (!BinkImportsLoaded) {
		return FALSE;
	}
	
    BinkHandle = 0;
    SurfaceFlags = 0;
    BinkVideoSurface = NULL;
    BinkVideoSurface = surface;
    VideoRect.left = 0;
    VideoRect.top = 0;
    VideoRect.right = 0;
    VideoRect.bottom = 0;
    FileHandle = INVALID_HANDLE_VALUE;
    IsPlaying = TRUE;
    NewFrame = FALSE;
    LastFrameNum = 0;

    return BinkMovie_Open(filename);
}


void __fastcall BinkMovie_Destroy(void)
{
	if (!BinkImportsLoaded) {
		return;
	}
	
    BinkMovie_Close();
}


BOOL __fastcall BinkMovie_ResumePause(void)
{
    BOOL playing = IsPlaying;

    if (GameInFocus) {
        if (!playing) {
            WWDebug_Printf("BinkMovie_ResumePause() - Resume bink movie.\n");
            IsPlaying = TRUE;
            BinkMovie_Pause(FALSE);
            BinkMovie_Draw_Frame();
        }

    } else if (playing) {
        WWDebug_Printf("BinkMovie_ResumePause() - Pause bink movie.\n");
        IsPlaying = FALSE;
        BinkMovie_Pause(TRUE);
    }

    return playing;
}


float __fastcall BinkMovie_Set_Master_Volume(float vol)
{
    float old = BinkMasterVolume;
    BinkMasterVolume = vol;
    return old;
}
