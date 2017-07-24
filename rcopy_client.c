// main() function for the client
#include <stdio.h>
#include <string.h>
#include "ftree.h"



int main(int argc, char **argv) {
    

    
    if (argc != 4) {
        printf("Usage:\n\trcopy_client SRC DEST HOST\n");
        return 0;
    }
    
    char buf[MAXPATH];
    fcopy_client(realpath(argv[1], buf), argv[2], argv[3], PORT);

    return 0;
}
