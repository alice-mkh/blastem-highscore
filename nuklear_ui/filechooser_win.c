#include <windows.h>
#include <stdint.h>

uint8_t native_filechooser_available(void)
{
	return 1;
}

char* native_filechooser_pick(const char *title, const char *start_directory)
{
	char file_name[MAX_PATH] = "";
	OPENFILENAMEA ofn = {
		.lStructSize = sizeof(ofn),
		.hwndOwner = NULL, //TODO: should probably get the HWND of the main window
		.lpstrFilter =
			"All Files\0*.*\0"
			"All Supported Types\0*.zip;*.bin;*.bin.gz;*.gen;*.gen.gz;*.md;*.md.gz;*.sms;*.sms.gz;*.gg;*.gg.gz;*.sg;*.sg.gz;*.cue;*.toc;*.flac;*.vgm;*.vgz;*.vgm.gz\0"
			"Genesis/MD\0.zip;*.bin;*.bin.gz;*.gen;*.gen*.gz;*.md;*.md.gz\0"
			"Sega/Mega CD\0*.cue;*.toc\0"
			"Sega 8-bit\0*.sms;*.sms.gz;*.gg;*.gg.gz;*.sg;*.sg.gz\0"
			"Audio/VGM\0*.flac;*.vgm;*.vgz;*.vgm.gz\0",
		.nFilterIndex = 2,
		.lpstrFile = file_name,
		.nMaxFile = sizeof(file_name),
		.lpstrInitialDir = start_directory,
		.lpstrTitle = title,
		.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_LONGNAMES,
	};
	char *ret = NULL;
	if (GetOpenFileNameA(&ofn)) {
		ret = strdup(file_name);
	}
	return ret;
}
