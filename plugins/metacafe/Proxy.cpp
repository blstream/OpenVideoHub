#include <e32std.h>
#include <ImplementationProxy.h>

#include <PluginUids.h>
#include "Plugin.h"

const TImplementationProxy ImplementationTable[] = 
	{
	IMPLEMENTATION_PROXY_ENTRY( 0xA0001878,	CPlugin::NewL )
	};

EXPORT_C const TImplementationProxy* ImplementationGroupProxy( TInt& aTableCount )
	{
	aTableCount = sizeof(ImplementationTable) / sizeof(TImplementationProxy);
	return ImplementationTable;
	}
