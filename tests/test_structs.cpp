// The real checks are the static_asserts in kadp_structs.h: if they fail, this does not compile.
#include <cstdio>

#include "kadp_structs.h"

int main() {
    static_assert(KADP_HDR_LEN == 40, "wire header is 40 B");
    static_assert((PAGE_REQUEST >> 4) == 0x1 && (KADP_ERROR >> 4) == 0xF, "class nibble");

    printf("PageEntry=%zu DirEntry=%zu HomeTxn=%zu QueuedReq=%zu\n", sizeof(PageEntry),
           sizeof(DirEntry), sizeof(HomeTxn), sizeof(QueuedReq));
    printf("FaultSlot=%zu NodeInfo=%zu AllocRecord=%zu NameEntry=%zu\n", sizeof(FaultSlot),
           sizeof(NodeInfo), sizeof(AllocRecord), sizeof(NameEntry));
    return 0;
}
