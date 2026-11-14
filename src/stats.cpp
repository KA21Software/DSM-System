#include "stats.h"

Counters& counters() {
    static Counters c;
    return c;
}
