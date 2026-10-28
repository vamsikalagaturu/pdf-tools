#include "pt_ranges.h"
#include <assert.h>
int main(void) {
    int a[30];
    assert(pt_parse_pages("3,1-2,6-4", 6, a, 30)==6);
    assert(a[0]==2 && a[1]==0 && a[2]==1 && a[3]==5 && a[4]==4 && a[5]==3);
    assert(pt_parse_pages("0", 6, a, 30)==-1);
    assert(pt_parse_pages("7", 6, a, 30)==-1);
    assert(pt_parse_pages("1-", 6, a, 30)==-1);
    assert(pt_parse_pages("1,", 6, a, 30)==-1);
    assert(pt_parse_pages("1-6", 6, a, 3)==-1);
    return 0;
}
