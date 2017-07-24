#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* hash(FILE *f) {
    if (!f) return NULL;
    
    int i = 0;
    int block_size = 8;
    char c = '\0';

    char* hash_val =  malloc(sizeof(char) * (block_size + 1));
    for (i = 0; i <= block_size; i++){
        hash_val[i] = '\0';
    }
    
    i = 0;


    while (fscanf(f, "%c", &c) == 1){
        hash_val[i % block_size] = hash_val[i % block_size] ^ c; 
        i++;
    }

    return hash_val;
}
