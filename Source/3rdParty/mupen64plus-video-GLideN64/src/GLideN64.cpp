#include <Revision.h>
char pluginName[] = "GLideN64-MPN";
#ifdef PLUGIN_REVISION
char pluginNameWithRevision[] = "GLideN64-MPN rev." PLUGIN_REVISION;
#else // PLUGIN_REVISION
char pluginNameWithRevision[] = "GLideN64-MPN";
#endif // PLUGIN_REVISION
wchar_t pluginNameW[] = L"GLideN64-MPN";
void (*CheckInterrupts)( void );
