// Preview-only stand-ins for the Home settings flags that HaConfig.cpp keeps in NVS.
#include <HaConfig.h>
namespace ha {
char (&pageOrder())[8] { static char order[8] = {'E','H','M','D', 0}; return order; }
bool &exportGraph() { static bool on = true; return on; }
bool &thinClock() { static bool on = false; return on; }
bool &hideBareMedia() { static bool on = false; return on; }
bool &lineInAsPhono() { static bool on = false; return on; }
}
