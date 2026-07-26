#pragma once

class ScreenSaver
{
public:
	// Prevents display sleep while a title is running.
	//
	// Implemented natively in ScreenSaverMac.mm. The previous implementation went
	// through SDL, which had to initialise SDL_INIT_VIDEO to do it -- inside a wx app
	// that already owns NSApplication. That is almost certainly the "feature crashes on
	// macOS" the config comment referred to, and it is also what forced SDL's event
	// pump onto the main thread.
	static void SetInhibit(bool inhibit);
};
