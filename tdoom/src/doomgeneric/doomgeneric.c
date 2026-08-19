#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"

pixel_t* DG_ScreenBuffer = NULL;

void *DG_AllocScreenBuffer(size_t bytes);
void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

	/* tdoom: DG_ScreenBuffer is deliberately left NULL. See I_FinishUpdate() --
	 * nothing writes to it any more, and port_video.c renders straight out of
	 * I_VideoBuffer. Allocating it would waste 64KB of internal SRAM. */

	DG_Init();

	D_DoomMain ();
}

